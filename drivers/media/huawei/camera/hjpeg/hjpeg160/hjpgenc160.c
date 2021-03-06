/*
 *  Hisilicon K3 SOC camera driver source file
 *
 *  Copyright (C) Huawei Technology Co., Ltd.
 *
 * Author:
 * Email:
 * Date:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/proc_fs.h>
#include <linux/wait.h>
#include <linux/videodev2.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>
#include <media/v4l2-fh.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/fs.h>
#include <linux/dma-mapping.h>
#include <media/huawei/hjpeg_cfg.h>
#include <asm/io.h>
#include <linux/hisi/hisi_ion.h>
#include <asm/uaccess.h>
#include <linux/rpmsg.h>
#include <linux/ioport.h>
#include <linux/hisi/hisi-iommu.h>

#include "hjpgenc160.h"
#include "hjpeg_intf.h"
#include "hjpg160_reg_offset.h"
#include "hjpg160_reg_offset_field.h"
#include "hjpg160_table.h"
#include "cam_log.h"
#include "hjpg160_debug.h"

#include "hjpgenc160_cs.h"
//lint -save -e740

#define SMMU 1
//#define SAMPLEBACK 1
//#define DUMP_FILE 1

#ifndef SMMU
/*TODO:use kernel dump mem space do JPEG IP system verify*/
#define JPEG_INPUT_PHYADDR       0x40000000
#define JPEG_OUTPUT_PHYADDR      0x50000000
#endif//SMMU

static void hjpeg160_isr_do_tasklet(unsigned long data);
DECLARE_TASKLET(hjpeg160_isr_tasklet, hjpeg160_isr_do_tasklet, (unsigned long)0);

typedef struct _tag_hjpeg160_hw_ctl
{
    void __iomem *                              base_viraddr;
    phys_addr_t                                 phyaddr;
    u32                                         mem_size;
    u32                                         irq_no;

    void __iomem *                              cvdr_viraddr;
    phys_addr_t                                 cvdr_phyaddr;
    u32                                         cvdr_mem_size;
}hjpeg160_hw_ctl_t;

typedef struct _tag_hjpeg160
{
    struct platform_device*                     pdev;
    hjpeg_intf_t                                intf;
    char const*                                 name;

    void __iomem *                              viraddr;
    phys_addr_t                                 phyaddr;
    u32                                         mem_size;
    u32                                         irq_no;
    struct semaphore                            buff_done;
    hjpeg160_hw_ctl_t                           hw_ctl;
    struct ion_client*                          ion_client;
    bool                                        power_on_state;

    u32                                         chip_type;
    struct regulator *                          jpeg_supply;
    struct clk *                                jpegclk[JPEG_CLK_MAX];
    unsigned int                                jpegclk_value[JPEG_CLK_MAX];
    unsigned int                                jpeg_power_ref;
    struct iommu_domain *                       domain;
    unsigned int                                phy_pgd_base;
} hjpeg160_t;

#define I2hjpeg160(i) container_of(i, hjpeg160_t, intf)

static int hjpeg160_encode_process(hjpeg_intf_t *i, void *cfg);
static int hjpeg160_power_on(hjpeg_intf_t *i);
static int hjpeg160_power_off(hjpeg_intf_t *i);
static int hjpeg160_get_reg(hjpeg_intf_t *i, void* cfg);
static int hjpeg160_set_reg(hjpeg_intf_t *i, void* cfg);

static void hjpeg160_get_dts(struct platform_device* pDev);
static void get_phy_pgd_base(struct device* pdev);

static int hjpeg_poweron(hjpeg160_t* pJpegDev);
static int hjpeg_poweroff(hjpeg160_t* pJpegDev);
static int hjpeg_need_powerup(unsigned int refs);
static int hjpeg_need_powerdn(unsigned int refs);

static void hjpeg160_do_smmu_config_cs(bool bypass);

static int hjpeg_setclk_enable(hjpeg160_t* pJpegDev, int idx);
static void hjpeg_clk_disable(hjpeg160_t* pJpegDev, int idx);

static void set_picture_format(void __iomem* base_addr, int fmt);
static void set_picture_quality(void __iomem* base_addr, unsigned int quality);

static hjpeg_vtbl_t
s_vtbl_hjpeg160 =
{
    .encode_process = hjpeg160_encode_process,
    .power_on = hjpeg160_power_on,
    .power_down = hjpeg160_power_off,
    .get_reg = hjpeg160_get_reg,
    .set_reg = hjpeg160_set_reg,
};

static hjpeg160_t
s_hjpeg160 =
{
    .intf = { .vtbl = &s_vtbl_hjpeg160, },
    .name = "hjpeg160",
};

static const struct of_device_id
s_hjpeg160_dt_match[] =
{
    {
        .compatible = "huawei,hjpeg160",
        .data = &s_hjpeg160.intf,
    },
    {
    },
};

static struct timeval s_timeval_start;
static struct timeval s_timeval_end;

MODULE_DEVICE_TABLE(of, s_hjpeg160_dt_match);

static void hjpeg160_isr_do_tasklet(unsigned long data)
{
    up(&s_hjpeg160.buff_done);
}

#define ENCODE_FINISH (1<<4)
static irqreturn_t hjpeg160_irq_handler(int irq, void *dev_id)
{
    void __iomem *base = s_hjpeg160.viraddr;
    u32 value = 0;

    do_gettimeofday(&s_timeval_end);
    value = REG_GET(base + JPGENC_JPE_STATUS_RIS_REG );
    if(value & ENCODE_FINISH) {
        tasklet_schedule(&hjpeg160_isr_tasklet);
    } else {
        cam_err("%s(%d) err irq status 0x%x ",__func__, __LINE__, value);
    }

    /*clr jpeg irq*/
    REG_SET((void __iomem*)((char*)base + JPGENC_JPE_STATUS_ICR_REG), 0x30);
    return IRQ_HANDLED;
}

static int hjpeg160_res_init(struct device *pdev)
{
    struct device_node *of_node = pdev->of_node;
    uint32_t base_array[2] = {0};
    uint32_t count = 0;
    int ret = -1;
    u32 chip_type = s_hjpeg160.chip_type;

    /* property(hisi,isp-base) = <address, size>, so count is 2 */
    count = 2;
    if (of_node) {
        ret = of_property_read_u32_array(of_node, "huawei,hjpeg160-base",
                base_array, count);
        if (ret < 0) {
            cam_err("%s failed line %d", __func__, __LINE__);
            return ret;
        }
    } else {
        cam_err("%s hjpeg160 of_node is NULL.%d", __func__, __LINE__);
        return -ENXIO;
    }

    s_hjpeg160.phyaddr = base_array[0];
    s_hjpeg160.mem_size = base_array[1];

    s_hjpeg160.viraddr = ioremap_nocache(s_hjpeg160.phyaddr, s_hjpeg160.mem_size);

    if (IS_ERR_OR_NULL(s_hjpeg160.viraddr)) {
        cam_err("%s ioremap fail", __func__);
        return -ENXIO;
    }

    s_hjpeg160.irq_no = irq_of_parse_and_map(of_node, 0);
    if (s_hjpeg160.irq_no  <= 0) {
        cam_err("%s failed line %d\n", __func__, __LINE__);
        goto fail;
    }

#if DUMP_REGS
    cam_info("%s hjpeg160 base address = 0x%x. hjpeg160-base size = 0x%x. hjpeg160-irq = %d viraddr = 0x%x",
            __func__,(void *)(s_hjpeg160.phyaddr), s_hjpeg160.mem_size, s_hjpeg160.irq_no, s_hjpeg160.viraddr);
#else
    cam_info("%s hjpeg160 base address = %pK. hjpeg160-base size = 0x%x. hjpeg160-irq = %d viraddr = %pK",
            __func__,(void *)(s_hjpeg160.phyaddr), s_hjpeg160.mem_size, s_hjpeg160.irq_no, s_hjpeg160.viraddr);
#endif


    /*request irq*/
    ret = request_irq(s_hjpeg160.irq_no, hjpeg160_irq_handler, 0, "hjpeg160_irq", 0);

    if (ret != 0) {
        cam_err("fail to request irq [%d], error: %d", s_hjpeg160.irq_no, ret);
        ret = -ENXIO;
        goto fail;
    }

    if (chip_type == CT_CS) {
        s_hjpeg160.hw_ctl.cvdr_viraddr = ioremap_nocache(REG_BASE_CVDR, CVDR_MEM_SIZE);
    } else {
        s_hjpeg160.hw_ctl.cvdr_viraddr = ioremap_nocache(CVDR_SRT_BASE, 4096);
    }
    if (IS_ERR_OR_NULL(s_hjpeg160.hw_ctl.cvdr_viraddr)){
        cam_err("%s (%d) remap cvdr viraddr failed",__func__, __LINE__);
        ret = -ENXIO;
        goto fail;
    }

#if (POWER_CTRL_INTERFACE==POWER_CTRL_CFG_REGS)
    if ((ret = cfg_map_reg_base())!=0) {
        ret = -ENXIO;
        goto fail;
    }
#endif//(POWER_CTRL_INTERFACE==POWER_CTRL_CFG_REGS)
    return 0;

fail:
    if (s_hjpeg160.viraddr){
        iounmap((void*)s_hjpeg160.viraddr);
        s_hjpeg160.viraddr = NULL;
    }

    if (s_hjpeg160.irq_no) {
        free_irq(s_hjpeg160.irq_no, 0);
        s_hjpeg160.irq_no = 0;
    }

    return ret;
}

static int hjpeg160_res_deinit(struct device *pdev)
{
    if (s_hjpeg160.viraddr){
        iounmap((void*)s_hjpeg160.viraddr);
        s_hjpeg160.viraddr = NULL;
    }

    if (s_hjpeg160.irq_no) {
        free_irq(s_hjpeg160.irq_no, 0);
        s_hjpeg160.irq_no = 0;
    }

    if(s_hjpeg160.hw_ctl.cvdr_viraddr)
        iounmap((void *)s_hjpeg160.hw_ctl.cvdr_viraddr);

#if (POWER_CTRL_INTERFACE==POWER_CTRL_CFG_REGS)
    cfg_unmap_reg_base();
#endif//(POWER_CTRL_INTERFACE==POWER_CTRL_CFG_REGS)

    return 0;
}

static int hjpeg160_enable_autogating(void)
{
    void __iomem*  base_addr = s_hjpeg160.viraddr;

    REG_SET((void __iomem*)((char*)base_addr + JPGENC_FORCE_CLK_ON_CFG_REG), 0x0);

    return 0;
}

static int hjpeg160_disable_autogating(void)
{
    void __iomem*  base_addr = s_hjpeg160.viraddr;

    REG_SET((void __iomem*)((char*)base_addr + JPGENC_FORCE_CLK_ON_CFG_REG), 0x1);

    return 0;
}


static void hjpeg160_hufftable_init(void)
{
    int tmp;
    uint32_t i;
    uint32_t length_bit,length_value,length;
    void __iomem* base_addr = s_hjpeg160.viraddr;


    /*DC 0 */
    length_bit = ARRAY_SIZE(luma_dc_bits);
    length_value = ARRAY_SIZE(luma_dc_value);
    length = length_bit + length_value;

    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TDC0_LEN_REG),length);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),4);
    for(i = 1;i < length_bit;i = i + 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,luma_dc_bits[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,luma_dc_bits[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }
    for(i = 1;i < length_value;i += 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,luma_dc_value[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,luma_dc_value[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }

    /*AC 0 */
    length_bit = ARRAY_SIZE(luma_ac_bits);
    length_value = ARRAY_SIZE(luma_ac_value);
    length = length_bit + length_value;
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TAC0_LEN_REG),length);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),5);
    for(i = 1;i < length_bit;i = i + 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,luma_ac_bits[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,luma_ac_bits[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }
    for(i = 1;i < length_value;i = i + 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,luma_ac_value[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,luma_ac_value[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }

    /*DC 1 */
    length_bit = ARRAY_SIZE(chroma_dc_bits);
    length_value = ARRAY_SIZE(chroma_dc_value);
    length = length_bit + length_value;
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TDC1_LEN_REG),length);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),6);
    for(i = 1;i < length_bit;i = i + 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,chroma_dc_bits[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,chroma_dc_bits[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }
    for(i = 1;i < length_value;i = i + 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,chroma_dc_value[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,chroma_dc_value[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }

    /*AC 1 */
    length_bit = ARRAY_SIZE(chroma_ac_bits);
    length_value = ARRAY_SIZE(chroma_ac_value);
    length = length_bit + length_value;
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TAC1_LEN_REG),length);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),7);
    for(i = 1;i < length_bit;i = i + 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,chroma_ac_bits[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,chroma_ac_bits[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }
    for(i = 1;i < length_value;i = i + 2){
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,chroma_ac_value[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,chroma_ac_value[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }
    /*end*/
}

static void hjpeg160_qtable_init(void)
{
    int length;
    int i;
    unsigned int tmp;
    void __iomem*  base_addr = s_hjpeg160.viraddr;


    /*q-table 0*/
    length = ARRAY_SIZE(luma_qtable1);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),0);
    for(i = 1;i < length;i = i + 2)
    {
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,luma_qtable1[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,luma_qtable1[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }

    /*q-table 1*/
    length = ARRAY_SIZE(chroma_qtable1);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),1);
    for(i = 1;i < length;i = i + 2)
    {
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,chroma_qtable1[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,chroma_qtable1[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }

    /*q-table 2*/
    length = ARRAY_SIZE(luma_qtable2);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),2);
    for(i = 1;i < length;i = i + 2)
    {
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,luma_qtable2[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,luma_qtable2[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }

    /*q-table 3*/
    length = ARRAY_SIZE(chroma_qtable2);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),3);
    for(i = 1;i < length;i = i + 2)
    {
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,chroma_qtable2[i - 1]);
        REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,chroma_qtable2[i]);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
    }
    /*end*/
}

static void hjpeg160_prefetch_init(void)
{
    uint32_t tmp = 0;
    void __iomem* base_addr = s_hjpeg160.viraddr;
    u32 chip_type = s_hjpeg160.chip_type;


#ifdef SMMU
    REG_SET_FIELD(tmp,JPGENC_PREFETCH_EN,1);
#else
    REG_SET_FIELD(tmp,JPGENC_PREFETCH_EN,0);
#endif

    REG_SET_FIELD(tmp,JPGENC_PREFETCH_DELAY,1210);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_PREFETCH_REG),tmp);

    if (chip_type == CT_ES) {
        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_ID0_Y,49);
        REG_SET_FIELD(tmp,JPGENC_ID1_Y,50);
        REG_SET_FIELD(tmp,JPGENC_ID2_Y,51);
        REG_SET_FIELD(tmp,JPGENC_ID3_Y,52);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_PREFETCH_IDY0_REG),tmp);

        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_ID4_Y,53);
        REG_SET_FIELD(tmp,JPGENC_ID5_Y,54);
        REG_SET_FIELD(tmp,JPGENC_ID6_Y,55);
        REG_SET_FIELD(tmp,JPGENC_ID7_Y,56);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_PREFETCH_IDY1_REG),tmp);

        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_ID0_UV,57);
        REG_SET_FIELD(tmp,JPGENC_ID1_UV,58);
        REG_SET_FIELD(tmp,JPGENC_ID2_UV,59);
        REG_SET_FIELD(tmp,JPGENC_ID3_UV,60);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_PREFETCH_IDUV0_REG),tmp);

        tmp = 0;
        REG_SET_FIELD(tmp,JPGENC_ID8_Y,61);
        REG_SET_FIELD(tmp,JPGENC_ID4_UV,62);
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_PREFETCH_IDUVY_REG),tmp);
    }
}

static void hjpeg160_rstmarker_init(void)
{
    void __iomem*  base_addr = s_hjpeg160.viraddr;

    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_RESTART_INTERVAL_REG),JPGENC_RESTART_INTERVAL);
}

static void hjpeg160_rstmarker(unsigned int rst)
{
    void __iomem*  base_addr = s_hjpeg160.viraddr;

    cam_info("%s enter , rst = %d",__func__, rst);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_RESTART_INTERVAL_REG),rst);
}

static void hjpeg160_synccfg_init(void)
{
    void __iomem* base_addr = s_hjpeg160.viraddr;
    uint32_t tmp       = 0;

    REG_SET_FIELD(tmp,JPGENC_SOURCE, 1);
    REG_SET_FIELD(tmp,JPGENC_SRAM_NOOPT, 1);
    REG_SET((void __iomem*)((char*)base_addr + JPGENC_SYNCCFG_REG),tmp);
}

static void hjpeg160_add_header(jpgenc_config_t* config)
{
    uint32_t i;
    int scaler;
    long temp;
    uint32_t length;
    uint32_t width;
    uint32_t height;
    uint32_t quality;
    encode_format_e format;
    unsigned int rst;
    unsigned char* jpgenc_addr;
    unsigned char* org_jpeg_addr;

    if (NULL == config) {
        cam_err("%s: config is NULL. (%d)",__func__, __LINE__);
        return;
    }

    length = 0;
    width = config->buffer.width;
    height = config->buffer.height;
    quality = config->buffer.quality;
    format = config->buffer.format;
    rst = config->buffer.rst;


#ifdef SMMU
    org_jpeg_addr = (unsigned char*)config->buffer.ion_vaddr;
#else
    org_jpeg_addr = ioremap_nocache(JPEG_OUTPUT_PHYADDR, JPGENC_HEAD_SIZE);
    if(NULL == org_jpeg_addr){
        cam_err("%s(%d) remap fail", __func__, __LINE__);
        return;
    }
#endif

    jpgenc_addr = org_jpeg_addr;

    *jpgenc_addr = JPGENC_HEAD_OFFSET;
    jpgenc_addr += JPGENC_HEAD_OFFSET;

    length = ARRAY_SIZE(header_soi);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = header_soi[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(header_app0);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = header_app0[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(header_qtable0);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = header_qtable0[i];
        jpgenc_addr ++;
    }

    if(quality == 0){
        length = ARRAY_SIZE(luma_qtable1);
        for(i = 0;i < length;i ++) {
            *jpgenc_addr = luma_qtable1[i];
            jpgenc_addr ++;
        }
    }else{
        if(quality < 50)
            scaler = 5000/quality;
        else
            scaler = 200 - quality*2;

        length = ARRAY_SIZE(luma_qtable2);
        for(i = 0; i < length; i ++){
            temp = (luma_qtable2[i] * scaler + 50L)/100L;
            if(temp <= 0L)
                temp = 1L;
            if(temp >255L)
                temp = 255L;
            *jpgenc_addr = temp;
            jpgenc_addr ++;
        }
    }

    length = ARRAY_SIZE(header_qtable1);
    for(i = 0; i < length; i ++){
        *jpgenc_addr = header_qtable1[i];
        jpgenc_addr ++;
    }

    if(quality == 0){
        length = ARRAY_SIZE(chroma_qtable1);
        for(i = 0;i < length;i ++){
            *jpgenc_addr = chroma_qtable1[i];
            jpgenc_addr ++;
        }
    }else{
        if(quality < 50)
            scaler = 5000/quality;
        else
            scaler = 200 - quality*2;

        length = ARRAY_SIZE(chroma_qtable2);
        for(i = 0;i < length;i ++) {
            temp = (chroma_qtable2[i]*scaler + 50L)/100L;
            if(temp <= 0L)
                temp = 1L;
            if(temp >255L)
                temp = 255L;
            *jpgenc_addr = temp;
            jpgenc_addr ++;
        }
    }

    *jpgenc_addr = 0xff;
    jpgenc_addr ++;
    *jpgenc_addr = 0xc0;
    jpgenc_addr ++;
    *jpgenc_addr = 0x00;
    jpgenc_addr ++;
    *jpgenc_addr = 0x11;
    jpgenc_addr ++;
    *jpgenc_addr = 0x08;
    jpgenc_addr ++;
    *jpgenc_addr = height/256;
    jpgenc_addr ++;
    *jpgenc_addr = height%256;
    jpgenc_addr ++;
    *jpgenc_addr = width/256;
    jpgenc_addr ++;
    *jpgenc_addr = width%256;
    jpgenc_addr ++;
    *jpgenc_addr = 0x03;
    jpgenc_addr ++;
    *jpgenc_addr = 0x01;
    jpgenc_addr ++;
    if(JPGENC_FORMAT_YUV422 == (format & JPGENC_FORMAT_BIT))
        *jpgenc_addr = 0x21;
    else
        *jpgenc_addr = 0x22;
    jpgenc_addr ++;
    *jpgenc_addr = 0x00;
    jpgenc_addr ++;

    *jpgenc_addr = 0x02;//0x02:nv12 0x03:nv21
    jpgenc_addr ++;
    *jpgenc_addr = 0x11;
    jpgenc_addr ++;
    *jpgenc_addr = 0x01;
    jpgenc_addr ++;
    *jpgenc_addr = 0x03;//0x03:nv12 0x02:nv21
    jpgenc_addr ++;
    *jpgenc_addr = 0x11;
    jpgenc_addr ++;
    *jpgenc_addr = 0x01;
    jpgenc_addr ++;

    length = ARRAY_SIZE(header_hufftable_dc0);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = header_hufftable_dc0[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(luma_dc_bits);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = luma_dc_bits[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(luma_dc_value);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = luma_dc_value[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(header_hufftable_ac0);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = header_hufftable_ac0[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(luma_ac_bits);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = luma_ac_bits[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(luma_ac_value);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = luma_ac_value[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(header_hufftable_dc1);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = header_hufftable_dc1[i];
        jpgenc_addr++;
    }

    length = ARRAY_SIZE(chroma_dc_bits);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = chroma_dc_bits[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(chroma_dc_value);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = chroma_dc_value[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(header_hufftable_ac1);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = header_hufftable_ac1[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(chroma_ac_bits);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = chroma_ac_bits[i];
        jpgenc_addr ++;
    }

    length = ARRAY_SIZE(chroma_ac_value);
    for(i = 0;i < length;i ++){
        *jpgenc_addr = chroma_ac_value[i];
        jpgenc_addr ++;
    }

    /* add DRI */
    *jpgenc_addr = 0xff;
    jpgenc_addr ++;
    *jpgenc_addr = 0xdd;
    jpgenc_addr ++;
    *jpgenc_addr = 0x00;
    jpgenc_addr ++;
    *jpgenc_addr = 0x04;
    jpgenc_addr ++;
    *jpgenc_addr = rst/256;
    jpgenc_addr ++;
    *jpgenc_addr = rst%256;
    jpgenc_addr ++;

    *jpgenc_addr = 0xff;
    jpgenc_addr ++;
    *jpgenc_addr = 0xda;
    jpgenc_addr ++;
    *jpgenc_addr = 0X00;
    jpgenc_addr ++;
    *jpgenc_addr = 0X0C;
    jpgenc_addr ++;
    *jpgenc_addr = 0x03;
    jpgenc_addr ++;
    *jpgenc_addr = 0x01;
    jpgenc_addr ++;
    *jpgenc_addr = 0x00;
    jpgenc_addr ++;
    *jpgenc_addr = 0x02;
    jpgenc_addr ++;
    *jpgenc_addr = 0x11;
    jpgenc_addr ++;
    *jpgenc_addr = 0x03;
    jpgenc_addr ++;
    *jpgenc_addr = 0x11;
    jpgenc_addr ++;
    *jpgenc_addr = 0x00;
    jpgenc_addr ++;
    *jpgenc_addr = 0x3F;
    jpgenc_addr ++;
    *jpgenc_addr = 0x00;

#ifndef SMMU
    if(org_jpeg_addr)
        iounmap((void *)org_jpeg_addr);
#endif

    return;
}

static int check_rst(jpgenc_config_t *config)
{
    int ret = 0;
    cam_debug("enter %s(%d)",__func__, __LINE__);

    if (NULL == config) {
        cam_err("%s: config is null! (%d)",__func__, __LINE__);
        return -EINVAL;
    }

    switch(config->buffer.rst) {
        case JPGENC_RESTART_INTERVAL:
            break;
        case JPGENC_RESTART_INTERVAL_ON:
            hjpeg160_rstmarker(config->buffer.rst);
            cam_info("JPEG restart interval is on %s(%d)", __func__, __LINE__);
            break;
        default:
            cam_err("invalid value of \"rst\". %s(%d)", __func__, __LINE__);
            ret = -1;
            config->buffer.rst = 0;
            break;
    }

    return ret;
}

static int check_config(jpgenc_config_t* config)
{
    cam_info("%s enter ",__func__);
    if(config == NULL){
        cam_err("%s: config is null! (%d)",__func__, __LINE__);
        return -EINVAL;
    }

    if(!CHECK_ALIGN(config->buffer.width,2) || (config->buffer.width > 8192)){
        cam_err(" width[%d] is invalid! ",config->buffer.width);
        return -1;
    }

    if(0 == config->buffer.height || (config->buffer.height > 8192)){
        cam_err(" height[%d] is invalid! ",config->buffer.height);
        return -1;
    }

    if((0 == config->buffer.stride)
            || !CHECK_ALIGN(config->buffer.stride,16)
            || (config->buffer.stride/16 > ((JPGENC_FORMAT_YUV422 == (config->buffer.format & JPGENC_FORMAT_BIT)) ? 1024 : 512)))
    {
        cam_err(" stride[%d] is invalid! ",config->buffer.stride);
        return -1;
    }

    if((0 == config->buffer.input_buffer_y) || !CHECK_ALIGN(config->buffer.input_buffer_y,16)){
        cam_err(" input buffer y[0x%x] is invalid! ",config->buffer.input_buffer_y);
        return -1;
    }
    if((JPGENC_FORMAT_YUV420 == (config->buffer.format & JPGENC_FORMAT_BIT))
            && ((0== config->buffer.input_buffer_uv )|| !CHECK_ALIGN(config->buffer.input_buffer_uv, 16))){
        cam_err(" input buffer uv[0x%x] is invalid! ",config->buffer.input_buffer_uv);
        return -1;
    }

    if((JPGENC_FORMAT_YUV420 == (config->buffer.format & JPGENC_FORMAT_BIT))
            && (config->buffer.input_buffer_uv - config->buffer.input_buffer_y < config->buffer.stride*8*16)){
        cam_err(" buffer format is invalid! ");
        return -1;
    }

    if(config->buffer.quality > 100){
        cam_err(" quality[%d] is invalid, adjust to 100",config->buffer.quality);
        config->buffer.quality = 100;
    }

    return 0;
}

static int hjpeg160_cvdr_fmt_desc_vp_wr(uint32_t width, uint32_t height,
    uint32_t axi_address_start, uint32_t buf_size, cvdr_wr_fmt_desc_t *desc)
{
    u32 chip_type = s_hjpeg160.chip_type;
    /* uint32_t pix_size = 64; */
    /* uint32_t mbits_per_sec = pix_size * g_isp_clk; */
    //FIXME:this must be use ceil align up
    /* uint32_t num_DUs_per_line = width * pix_size / 8 / 128; */
    /* uint32_t total_num_bytes  = num_DUs_per_line * height * 128; */
    uint32_t total_num_bytes = buf_size;
    if(0 == width){
       cam_err("width cannot be zero");
       return -1;
    }

    desc->pix_fmt        = DF_D64;
    desc->pix_expan      = EXP_PIX;
    //FIXME: this must bue user ceil to align up float to int
    /* desc->access_limiter = 32 * pix_size / 128  ; */
    desc->access_limiter = 16;
    desc->last_page      = (axi_address_start + total_num_bytes)>>15;
    desc->line_stride    = (chip_type==CT_CS) ? 0x3F : ((width * 2) / 16 - 1);
    desc->line_wrap      = (chip_type==CT_CS) ? 0x3FFF : height;

    cam_info("%s acess_limiter = %d, last_page = %d, line_stride = %d, width = %d,  height = %d",
        __func__, desc->access_limiter, desc->last_page, desc->line_stride, width, height);

    return 0;
}

#define PREFETCH_BY_PASS (1 << 31)
static int hjpeg160_set_vp_wr_ready(cvdr_wr_fmt_desc_t *desc, uint32_t buf_addr)
{
    void __iomem *cvdr_srt_base = s_hjpeg160.hw_ctl.cvdr_viraddr;//ioremap_nocache();
    int ret = 0;
    U_VP_WR_CFG tmp_cfg;
    U_VP_WR_AXI_LINE tmp_line;
    U_VP_WR_AXI_FS tmp_fs;
    u32 chip_type = s_hjpeg160.chip_type;

    u32 VP_WR_CFG_OFFSET = (chip_type==CT_CS)
                            ? CVDR_AXI_JPEG_VP_WR_CFG_0_OFFSET
                            : CVDR_SRT_VP_WR_CFG_25_OFFSET;
    u32 VP_WR_AXI_LINE_OFFSET = (chip_type==CT_CS)
                            ? CVDR_AXI_JPEG_VP_WR_AXI_LINE_0_OFFSET
                            : CVDR_SRT_VP_WR_AXI_LINE_25_OFFSET;
#ifndef SMMU
    u32 VP_WR_IF_CFG = (chip_type==CT_CS)
                            ? CVDR_AXI_JPEG_VP_WR_IF_CFG_0_OFFSET
                            : CVDR_SRT_VP_WR_IF_CFG_25_OFFSET;
#endif
    u32 VP_WR_AXI_FS = (chip_type==CT_CS)
                            ? CVDR_AXI_JPEG_VP_WR_AXI_FS_0_OFFSET
                            : CVDR_SRT_VP_WR_AXI_FS_25_OFFSET;

    tmp_cfg.reg32 = REG_GET(cvdr_srt_base + VP_WR_CFG_OFFSET);
    tmp_cfg.bits.vpwr_pixel_format = desc->pix_fmt;
    tmp_cfg.bits.vpwr_pixel_expansion = desc->pix_expan;
    if (chip_type==CT_ES) {
        tmp_cfg.bits.vpwr_access_limiter = desc->access_limiter;
    }
    tmp_cfg.bits.vpwr_last_page = desc->last_page;
    REG_SET((void __iomem*)((char*)cvdr_srt_base + VP_WR_CFG_OFFSET), tmp_cfg.reg32);

    tmp_line.reg32 = REG_GET(cvdr_srt_base + VP_WR_AXI_LINE_OFFSET);
    tmp_line.bits.vpwr_line_stride = desc->line_stride;
    tmp_line.bits.vpwr_line_wrap = desc->line_wrap;
    REG_SET((void __iomem*)((char*)cvdr_srt_base + VP_WR_AXI_LINE_OFFSET), tmp_line.reg32);

    if (chip_type==CT_CS) {
        REG_SET((void __iomem*)((char*)cvdr_srt_base + CVDR_AXI_JPEG_LIMITER_VP_WR_0_OFFSET), 0x0F00FFFF);
    }
#ifndef SMMU
    REG_SET((void __iomem*)((char*)cvdr_srt_base + VP_WR_IF_CFG),
        (REG_GET((void __iomem*)((char*)cvdr_srt_base + VP_WR_IF_CFG))|(PREFETCH_BY_PASS )));
#endif

    tmp_fs.reg32 = REG_GET((void __iomem*)((char*)cvdr_srt_base + VP_WR_AXI_FS));
    tmp_fs.bits.vpwr_address_frame_start = buf_addr >> 4;
    REG_SET((void __iomem*)((char*)cvdr_srt_base + VP_WR_AXI_FS), tmp_fs.reg32);
    return ret;
}

static int hjpeg160_set_nr_rd_config(unsigned char du, unsigned char limiter)
{
    void __iomem *cvdr_srt_base = s_hjpeg160.hw_ctl.cvdr_viraddr;//ioremap_nocache();
    U_CVDR_SRT_NR_RD_CFG_1 tmp;
    U_CVDR_SRT_LIMITER_NR_RD_1 lmt;
    u32 chip_type = s_hjpeg160.chip_type;

    u32 NR_RD_CFG = (chip_type==CT_CS) ? CVDR_AXI_JPEG_NR_RD_CFG_0_OFFSET : CVDR_SRT_NR_RD_CFG_1_OFFSET;
    u32 LIMITER_NR_RD = (chip_type==CT_CS) ? CVDR_AXI_JPEG_LIMITER_NR_RD_0_OFFSET : CVDR_SRT_LIMITER_NR_RD_1_OFFSET;

    tmp.reg32 = REG_GET( cvdr_srt_base + NR_RD_CFG );
    tmp.bits.nrrd_allocated_du_1 = du;
    tmp.bits.nrrd_enable_1 = 1;
    REG_SET((void __iomem*)((char*)cvdr_srt_base + NR_RD_CFG) , tmp.reg32);

    lmt.reg32 = REG_GET( cvdr_srt_base + LIMITER_NR_RD );
    lmt.bits.nrrd_access_limiter_0_1 = limiter;
    lmt.bits.nrrd_access_limiter_1_1 = limiter;
    lmt.bits.nrrd_access_limiter_2_1 = limiter;
    lmt.bits.nrrd_access_limiter_3_1 = limiter;
    lmt.bits.nrrd_access_limiter_reload_1 = 0xF;
    REG_SET((void __iomem*)((char*)cvdr_srt_base + LIMITER_NR_RD) , lmt.reg32);

    return 0;
}

static void hjpeg160_do_cvdr_config(jpgenc_config_t* config)
{
    int ret = 0;
    uint32_t width;
    uint32_t height;
    uint32_t buf_addr;
    uint32_t buf_size;
    cvdr_wr_fmt_desc_t cvdr_wr_fmt;
    u32 chip_type = s_hjpeg160.chip_type;
    //FIXME:use ceil align up float to int
    unsigned char access_limiter = ACCESS_LIMITER;
    unsigned char allocated_du = (chip_type==CT_CS) ? 6 : ALLOCATED_DU;

    cam_info("%s enter ",__func__);
    if (NULL == config) {
        cam_err("%s: config is null! (%d)",__func__, __LINE__);
        return;
    }
    width = config->buffer.width;
    height = config->buffer.height;
    buf_addr = config->buffer.output_buffer + JPGENC_HEAD_SIZE;
    buf_size = config->buffer.output_size - JPGENC_HEAD_SIZE;

    ret =  hjpeg160_cvdr_fmt_desc_vp_wr(width, height, buf_addr, buf_size, &cvdr_wr_fmt);
    if(0 != ret){
        cam_err("%s (%d) config cvdr failed", __func__, __LINE__);
        return;
    }

    ret = hjpeg160_set_vp_wr_ready(&cvdr_wr_fmt, buf_addr);
    if(0 != ret){
        cam_err("%s (%d) set vp wr ready fail", __func__, __LINE__);
    }

    hjpeg160_set_nr_rd_config(allocated_du, access_limiter);
}

static void hjpeg160_do_config(jpgenc_config_t* config)
{
    uint32_t width_left;
    uint32_t width_right = 0;
    void __iomem*  base_addr = s_hjpeg160.viraddr;

    u32 chip_type = s_hjpeg160.chip_type;

    cam_info("%s enter ",__func__);
    if (NULL == config) {
        cam_err("%s: config is null! (%d)",__func__, __LINE__);
        return;
    }

    set_picture_format(base_addr, config->buffer.format);

    //set picture size
    if (chip_type == CT_CS) {
        width_left = ALIGN_UP(config->buffer.width, 16);
    } else {
        width_left  = config->buffer.width;
        if(width_left >= 64)
        {
            width_left = ALIGN_DOWN((width_left/2), 16);
        }
        width_right = config->buffer.width - width_left;
    }
    SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_ENC_HRIGHT1_REG),JPGENC_ENC_HRIGHT1,width_left -1);
    SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_ENC_VBOTTOM_REG),JPGENC_ENC_VBOTTOM,config->buffer.height - 1);
    SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_ENC_HRIGHT2_REG),JPGENC_ENC_HRIGHT2,width_right != 0 ? width_right -1 : 0);

    set_picture_quality(base_addr, config->buffer.quality);

    //set input buffer address
    SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_ADDRESS_Y_REG),JPGENC_ADDRESS_Y,config->buffer.input_buffer_y >> 4);

    if(JPGENC_FORMAT_YUV420 == (config->buffer.format & JPGENC_FORMAT_BIT)) {
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_ADDRESS_UV_REG),JPGENC_ADDRESS_UV,config->buffer.input_buffer_uv >> 4);
    }

    //set preread
    if(JPGENC_FORMAT_YUV420 == (config->buffer.format & JPGENC_FORMAT_BIT))
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_PREREAD_REG),JPGENC_PREREAD,4);
    else
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_PREREAD_REG),JPGENC_PREREAD,0);

    SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_STRIDE_REG),JPGENC_STRIDE,config->buffer.stride >> 4);
    SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_ENCODE_REG),JPGENC_ENCODE,1);

    cam_info("%s activate JPGENC",__func__);
    do_gettimeofday(&s_timeval_start);
    SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_INIT_REG),JPGENC_JP_INIT,1);
}

static int hjpeg160_prepare_buf(jpgenc_config_t *cfg)
{
    struct ion_handle* hdl;
    void *vaddr;

    if (NULL == cfg) {
        cam_err("%s: cfg is null! (%d)",__func__, __LINE__);
        return -EINVAL;
    }

    /* check arg */
    if ((cfg->buffer.ion_fd < 0) || IS_ERR_OR_NULL(s_hjpeg160.ion_client)) {
        cam_err("invalid ion: fd=%d, client=%pK",
                cfg->buffer.ion_fd, s_hjpeg160.ion_client);
        return -EINVAL;
    }
    cam_info("%s: cfg->buffer.ion_fd is %d",__func__, cfg->buffer.ion_fd);

    /* get hdl */
    hdl = ion_import_dma_buf(s_hjpeg160.ion_client, cfg->buffer.ion_fd);
    if (IS_ERR_OR_NULL(hdl)) {
        cam_err("failed to create ion handle!");
        return PTR_ERR(hdl);
    }
    cfg->buffer.ion_hdl  = hdl;

    /* map kernel addr */
    vaddr = ion_map_kernel(s_hjpeg160.ion_client, hdl);
    if (IS_ERR_OR_NULL(vaddr)) {
        cam_err("failed to map ion buffer(fd=%lx)!", (unsigned long)hdl);
        cfg->buffer.ion_hdl = NULL;
        cfg->buffer.ion_vaddr = NULL;
        ion_free(s_hjpeg160.ion_client, hdl);
        return PTR_ERR(vaddr);
    }
#if DUMP_REGS
    cam_info("%s(%d) vaddr is 0x%x hdl = 0x%x",__func__, __LINE__, vaddr, hdl);
#else
    cam_info("%s(%d) vaddr is %pK hdl = %pK",__func__, __LINE__, vaddr, hdl);
#endif
    cfg->buffer.ion_vaddr = vaddr;

    return 0;
}

#ifndef SMMU
static int hjpeg160_cfg_smmu(bool bypass)
{
    void __iomem* smmu_base_addr;
    cam_info("%s enter ",__func__);
    if(true ==  bypass){
        smmu_base_addr = ioremap_nocache(SMMU_BASE_ADDR, 8192);

        if(IS_ERR_OR_NULL(smmu_base_addr)){
            cam_err("%s(%d) remap failed",__func__, __LINE__);
            return -1;
        }
        /* disable SMMU for global */
        //REG_SET(smmu_base_addr + SMMU_GLOBAL_BYPASS, REG_GET(smmu_base_addr + SMMU_GLOBAL_BYPASS)|0x1) ;
        /* disable SMMU only for JPGENC */
        REG_SET((void __iomem*)((char*)smmu_base_addr + SMMU_BYPASS_VPWR), REG_GET((void __iomem*)((char*)smmu_base_addr + SMMU_BYPASS_VPWR))|0x1) ;
        REG_SET((void __iomem*)((char*)smmu_base_addr + SMMU_BYPASS_NRRD1), REG_GET((void __iomem*)((char*)smmu_base_addr + SMMU_BYPASS_NRRD1))|0x1) ;
        REG_SET((void __iomem*)((char*)smmu_base_addr + SMMU_BYPASS_NRRD2), REG_GET((void __iomem*)((char*)smmu_base_addr + SMMU_BYPASS_NRRD2))|0x1) ;
        iounmap(smmu_base_addr);
    }

    return 0;
}

char *image_file_addr;
u32 image_file_size;
static int load_image_file(char *filename)
{
    struct kstat stat;
    mm_segment_t fs;
    struct file *fp = NULL;
    int file_flag = O_RDONLY;
    int ret = 0;

    cam_info("%s enter ",__func__);

    if (NULL == filename) {
        cam_err("%s param error", __func__);
        return -EINVAL;
    }

    /* must have the following 2 statement */
    fs = get_fs();
    set_fs(KERNEL_DS);
    cam_info("%s (%d) opening file %s", __func__, __LINE__, filename);
    fp = filp_open(filename, file_flag, 0666);
    if (IS_ERR_OR_NULL(fp)) {
        cam_err("open file error!\n");
        set_fs(fs);
        return -ENOENT;
    }

    if (0 != vfs_stat(filename, &stat)) {
        cam_err("failed to get file stat!");
        ret = -EIO;
        goto ERROR;
    }

    image_file_size = stat.size;
    cam_notice("image file %s, file size : %d", filename, (u32) stat.size);
    ret = vfs_read(fp, (char *)image_file_addr, (u32) image_file_size, &fp->f_pos);
    if (ret != stat.size) {
        cam_err("read file error!, %s , ret=%d\n", filename, ret);
        ret = -EIO;
    } else {
        ret = 0;
    }

ERROR:
    /* must have the following 1 statement */
    set_fs(fs);
    filp_close(fp, 0);
    return ret;
}
#endif

static void hjpeg160_disabe_irq(void)
{
    void __iomem *base = s_hjpeg160.viraddr;
    REG_SET((void __iomem*)((char*)base + JPGENC_JPE_STATUS_IMR_REG), 0x00);
}

static void hjpeg160_enable_irq(void)
{
    void __iomem *base = s_hjpeg160.viraddr;
    REG_SET((void __iomem*)((char*)base + JPGENC_JPE_STATUS_IMR_REG), 0x30);
}

static void hjpeg160_calculate_encoding_time(void)
{
    u64 tm_used = 0;

    tm_used = (s_timeval_end.tv_sec - s_timeval_start.tv_sec) * MICROSECOND_PER_SECOND
        + (s_timeval_end.tv_usec - s_timeval_start.tv_usec);

    cam_info("%s JPGENC encoding elapse %llu us",__func__, tm_used);
}

static int hjpeg160_encode_process(hjpeg_intf_t *i, void *cfg)
{
    jpgenc_config_t* pcfg;
    long   jiff = 0;
    int    ret = 0;
    u32 byte_cnt = 0;
#ifndef SMMU
    unsigned long addr = 0;
#endif
    u32 chip_type = s_hjpeg160.chip_type;

    cam_info("%s enter ",__func__);

    if (NULL == cfg) {
        cam_err("%s: cfg is null! (%d)",__func__, __LINE__);
        return -EINVAL;
    }
    pcfg = (jpgenc_config_t *)cfg;

    cam_info("width:%d, height:%d, stride:%d, format:%#x, quality:%d, rst:%d, ion_fd:%d",
            pcfg->buffer.width,
            pcfg->buffer.height,
            pcfg->buffer.stride,
            pcfg->buffer.format,
            pcfg->buffer.quality,
            pcfg->buffer.rst,
            pcfg->buffer.ion_fd);
    cam_info("input_buffer_y:%#x, input_buffer_uv:%#x, output_buffer:%#x, output_size:%u",
            pcfg->buffer.input_buffer_y,
            pcfg->buffer.input_buffer_uv,
            pcfg->buffer.output_buffer,
            pcfg->buffer.output_size);

    if (false == s_hjpeg160.power_on_state) {
        cam_err("%s(%d)jpegenc is not powered on, encode processing terminated.",__func__, __LINE__);
        return -1;
    }

#ifdef SMMU
    if ((0 == s_hjpeg160.phy_pgd_base) && (chip_type==CT_CS)) {
        cam_err("%s(%d)phy_pgd_base is invalid, encode processing terminated.",__func__, __LINE__);
    }
#endif

    if (0 != check_rst(pcfg)) {
        cam_err("%s(%d)checking rst failed, adjust to 0.",__func__, __LINE__);
    }

#ifndef SAMPLEBACK
    ret = check_config(pcfg);
    if(0 != ret){
        cam_err("%s(%d)check_config failed, encode processing terminated.",__func__, __LINE__);
        return ret;
    }
#endif

#ifndef SMMU
    image_file_addr = ioremap_nocache(JPEG_INPUT_PHYADDR, (pcfg->buffer.width * pcfg->buffer.height) * 2);

    if(IS_ERR_OR_NULL(image_file_addr)){
        cam_err("%s(%d) remap failed",__func__, __LINE__);
        return -1;
    }

    load_image_file(&(pcfg->filename));
#endif

#ifdef SMMU
    if (chip_type==CT_CS) {
        hjpeg160_do_smmu_config_cs(false);
    }
    ret = hjpeg160_prepare_buf(pcfg);
    if(0 != ret){
        cam_err("%s(%d)prepare buffer failed, encode processing terminated.",__func__, __LINE__);
        return ret;
    }
#else
    if (chip_type==CT_CS) {
        hjpeg160_do_smmu_config_cs(true);
    } else {
        hjpeg160_cfg_smmu(true);
    }
#endif

    hjpeg160_disable_autogating();
    hjpeg160_do_cvdr_config(pcfg);
    hjpeg160_enable_irq();

    hjpeg160_do_config(pcfg);

    jiff = msecs_to_jiffies(WAIT_ENCODE_TIMEOUT);
    if (down_timeout(&s_hjpeg160.buff_done, jiff)) {
        cam_err("time out wait for jpeg encode");
        ret = -1;
    }

    hjpeg160_calculate_encoding_time();

#if DUMP_REGS
    dump_cvdr_reg(chip_type, s_hjpeg160.hw_ctl.cvdr_viraddr);
    dump_jpeg_reg(chip_type, s_hjpeg160.viraddr);
#endif//DUMP_REGS

    hjpeg160_disabe_irq();
    hjpeg160_enable_autogating();

    hjpeg160_add_header(pcfg);

    byte_cnt = REG_GET((void __iomem*)((char*)s_hjpeg160.viraddr + JPGENC_JPG_BYTE_CNT_REG));
    if(0 == byte_cnt){
        cam_err("%s encode fail,  byte cnt [%u]", __func__, byte_cnt);
#if DUMP_ERR_DATA
        pcfg->jpegSize = pcfg->buffer.width * pcfg->buffer.height * 2;
        ret = 0;
        cam_info("%s set jpeg size as %d for dump err data.", __func__, pcfg->jpegSize);
#else
        pcfg->jpegSize = 0;
        ret = -ENOMEM;
        goto error;
#endif
    }else{
        byte_cnt += JPGENC_HEAD_SIZE;
    }
    if (byte_cnt > pcfg->buffer.output_size) {
        cam_err("%s encode fail, output buffer overflowed! byte cnt [%u] > buffer size [%u] ", __func__,
            byte_cnt, pcfg->buffer.output_size);
        ret = -ENOMEM;
        goto error;
    }
    pcfg->jpegSize = byte_cnt - JPGENC_HEAD_OFFSET;

    cam_info("%s user jpeg size is %u ", __func__, pcfg->jpegSize);
    cam_info("%s jpeg encode process success",__func__);

error:
#ifdef SMMU
    ion_unmap_kernel(s_hjpeg160.ion_client, pcfg->buffer.ion_hdl);
    ion_free(s_hjpeg160.ion_client, pcfg->buffer.ion_hdl);

#else
    addr = ioremap_nocache(JPEG_OUTPUT_PHYADDR, pcfg->buffer.width * pcfg->buffer.height * 2);
    //hjpeg160_dump_file("/data/img/out.jpg", addr, byte_cnt);
    mb();

    cam_info("%s(%d) start to memcpy output, ", __func__, __LINE__);
    memcpy((unsigned char*)pcfg->buffer.ion_vaddr, addr, pcfg->jpegSize);
    ion_unmap_kernel(s_hjpeg160.ion_client, pcfg->buffer.ion_hdl);
    ion_free(s_hjpeg160.ion_client, pcfg->buffer.ion_hdl);

    iounmap((void *)addr);
    iounmap(image_file_addr);
#endif

    return ret;
}

void hjpeg160_init_hw_param(void)
{
    hjpeg160_hufftable_init();

    hjpeg160_qtable_init();

    hjpeg160_prefetch_init();

    hjpeg160_rstmarker_init();

    hjpeg160_synccfg_init();
}

static inline void hjpeg160_set_power_reg(unsigned long reg,unsigned int  value)
{
    void __iomem* regaddr;

    regaddr = ioremap_nocache(reg, 0x4);

    REG_SET(regaddr, value);

    iounmap(regaddr);
}

static int hjpeg160_clk_ctrl(bool flag)
{
    void __iomem* subctrl1;
    uint32_t set_clk;
    uint32_t cur_clk;
    int ret = 0;
    u32 chip_type = s_hjpeg160.chip_type;

    cam_info("%s enter\n",__func__);

    if (chip_type==CT_CS) {
        subctrl1 = ioremap_nocache(REG_BASE_TOP, 0x4);
    } else {
        subctrl1 = ioremap_nocache(ISP_CORE_CTRL_1_REG, 0x4);
    }

    if (flag)
        REG_SET(subctrl1, REG_GET(subctrl1)|0x1);
    else
        REG_SET(subctrl1, REG_GET(subctrl1)&0xFFFFFFFE);

    set_clk = flag ? 0x1 : 0x0;
    cur_clk = REG_GET(subctrl1);
    if (set_clk != cur_clk) {
        cam_err("%s(%d) isp jpeg clk status %d, clk write failed",__func__, __LINE__, cur_clk);
        ret = -EIO;
    }

    cam_info("%s isp jpeg clk status %d",__func__, cur_clk);
    iounmap(subctrl1);
    return ret;
}

static int hjpeg160_power_on(hjpeg_intf_t *i)
{
    int ret = 0;
    hjpeg160_t* phjpeg160 = NULL;

    cam_info("%s enter\n",__func__);

    phjpeg160 = I2hjpeg160(i);

    if(false == phjpeg160->power_on_state ){
        ret = hjpeg160_res_init(&phjpeg160->pdev->dev);
        if(ret != 0){
            cam_err("%s(%d) res init fail",__func__, __LINE__);
            return ret;
        }

        if (phjpeg160->chip_type == CT_CS) {
            ret = hjpeg_poweron(phjpeg160);
        } else {
            ret = hisp_jpeg_powerup();
        }

        if(0 != ret){
            cam_err("%s(%d) jpeg power up fail",__func__, __LINE__);
            goto POWERUP_ERROR;
        }

        ret = hjpeg160_clk_ctrl(true);
        if (0 != ret) {
            cam_err("%s(%d) failed to enable jpeg clock , prepare to power down!",__func__, __LINE__);
            goto FAILED_RET;
        }

        hjpeg160_init_hw_param();
        sema_init(&(phjpeg160->buff_done), 0);

        s_hjpeg160.ion_client = hisi_ion_client_create("hwcam-hjpeg");
        if (IS_ERR_OR_NULL(s_hjpeg160.ion_client )) {
            cam_err("failed to create ion client! \n");
            ret = -ENOMEM;
            goto FAILED_RET;
        }
        phjpeg160->power_on_state = true;
        cam_info("%s jpeg power on success",__func__);
    }
    return ret;

FAILED_RET:
    if (phjpeg160->chip_type == CT_CS) {
        ret = hjpeg_poweroff(phjpeg160);
    } else {
        ret = hisp_jpeg_powerdn();
    }
    if (0!=ret)
        cam_err("%s(%d) jpeg power down fail",__func__, __LINE__);

POWERUP_ERROR:
    if (0 != hjpeg160_res_deinit(&phjpeg160->pdev->dev))
        cam_err("%s(%d) res deinit fail",__func__, __LINE__);

    return ret;
}

static int hjpeg160_power_off(hjpeg_intf_t *i)
{
    int ret = 0;
    hjpeg160_t* phjpeg160;
    struct ion_client*  ion = NULL;

    phjpeg160 = I2hjpeg160(i);

    if(true == phjpeg160->power_on_state){

        swap(s_hjpeg160.ion_client, ion);
        if (ion) {
            ion_client_destroy(ion);
        }

        if (phjpeg160->chip_type == CT_CS) {
            ret = hjpeg_poweroff(phjpeg160);
        } else {
            ret = hisp_jpeg_powerdn();
        }
        if(ret != 0){
            cam_err("%s jpeg power down fail",__func__);
        }

        if (0 != hjpeg160_res_deinit(&phjpeg160->pdev->dev)) {
            cam_err("%s(%d) res deinit fail",__func__, __LINE__);
        }

        phjpeg160->power_on_state = false;
        if (0 == ret)
            cam_info("%s jpeg power off success",__func__);
    }

    return ret;
}

static int hjpeg160_set_reg(hjpeg_intf_t* i, void* cfg)
{
    int ret = 0;

#ifdef SAMPLEBACK
    hjpeg160_t* phjpeg160   = NULL;
    void __iomem* base_addr = 0;
    jpgenc_config_t* pcfg = NULL;
    uint32_t addr;
    uint32_t value;


    cam_info("%s enter\n",__func__);

    if(s_hjpeg160.power_on_state == false){
        cam_err("%s(%d)jpgenc is not powered on.",__func__, __LINE__);
        return -1;
    }

    phjpeg160 = I2hjpeg160(i);
    base_addr = phjpeg160->viraddr;

    if (NULL == cfg) {
        cam_err("%s: cfg is null! (%d)",__func__, __LINE__);
        return -EINVAL;
    }
    pcfg = (jpgenc_config_t *)cfg;
    addr = pcfg->reg.addr;
    value = pcfg->reg.value;

    if((addr < JPGENC_JPE_ENCODE_REG)
            ||(addr > JPGENC_FORCE_CLK_ON_CFG_REG )){
        cam_info("%s input addr is invaild 0x%x\n",__func__, addr);
        ret = -1;
        return ret;
    }

    REG_SET(base_addr + addr, value);

    cam_info("%s input addr is  0x%x input value is %d\n",__func__, addr, value);
#endif

    return ret;
}

static int hjpeg160_get_reg(hjpeg_intf_t *i, void* cfg)
{
    int ret = 0;

#ifdef SAMPLEBACK
    hjpeg160_t* phjpeg160   = NULL;
    void __iomem* base_addr = 0;
    jpgenc_config_t* pcfg = NULL;
    uint32_t addr;


    cam_info("%s enter\n",__func__);
    if(s_hjpeg160.power_on_state == false){
        cam_err("%s(%d)jpgenc is not powered on.",__func__, __LINE__);
        return -1;
    }

    phjpeg160 = I2hjpeg160(i);
    base_addr = phjpeg160->viraddr;

    if (NULL == cfg) {
        cam_err("%s: cfg is null! (%d)",__func__, __LINE__);
        return -EINVAL;
    }
    pcfg = (jpgenc_config_t *)cfg;
    addr = pcfg->reg.addr;

    if((addr < JPGENC_JPE_ENCODE_REG)
            ||(addr > JPGENC_FORCE_CLK_ON_CFG_REG )){
        cam_info("%s input addr is invaild 0x%x\n",__func__, addr);
        ret = -1;
        return ret;
    }

    pcfg->reg.value = REG_GET(base_addr + addr);

    cam_info("%s input addr is  0x%x input value is %d\n",__func__, addr, pcfg->reg.value);
#endif

    return ret;
}

static int hjpeg_poweron(hjpeg160_t* pJpegDev)
{
    int ret = 0;
    cam_info("%s enter\n",__func__);

    if (!hjpeg_need_powerup(pJpegDev->jpeg_power_ref)) {
        pJpegDev->jpeg_power_ref++;
        cam_info("%s: jpeg power up, ref=%d\n", __func__, pJpegDev->jpeg_power_ref);
        return 0;
    }

#if (POWER_CTRL_INTERFACE==POWER_CTRL_CFG_REGS)
    //power up with config regs
    if ((ret = cfg_powerup_regs())!=0) {
        cam_err("Failed: cfg_powerup_regs.%d\n", ret);
        goto jpeg_err;
    }
#else
    //power up with hardware interface
    if ((ret = regulator_enable(pJpegDev->jpeg_supply)) != 0) {
        cam_err("Failed: regulator_enable.%d\n", ret);
        goto jpeg_err;
    }
#endif

    if ((ret = hjpeg_setclk_enable(pJpegDev, JPEG_FUNC_CLK)) != 0) {
        cam_err("Failed: ispfunc_setclk_enable.%d\n", ret);
        goto jpeg_err;
    }

    pJpegDev->jpeg_power_ref++;
    cam_info("%s: jpeg power up, ref=%d\n", __func__, pJpegDev->jpeg_power_ref);

    return ret;

jpeg_err:
#if (POWER_CTRL_INTERFACE==POWER_CTRL_CFG_REGS)
    cfg_powerdn_regs();
#else
    if ((ret = regulator_disable(pJpegDev->jpeg_supply)) != 0)
        cam_err("Failed: regulator_disable.%d\n", ret);
#endif

    return ret;
}
static int hjpeg_poweroff(hjpeg160_t* pJpegDev)
{
    int ret = 0;

    cam_info("%s enter\n",__func__);

    if (!hjpeg_need_powerdn(pJpegDev->jpeg_power_ref)) {
        pJpegDev->jpeg_power_ref--;
        cam_info("%s: jpeg power down, ref=%d\n", __func__, pJpegDev->jpeg_power_ref);
        return 0;
    }

    hjpeg_clk_disable(pJpegDev, JPEG_FUNC_CLK);

#if (POWER_CTRL_INTERFACE==POWER_CTRL_CFG_REGS)
    //power down with config regs
    if ((ret = cfg_powerdn_regs())!=0) {
        cam_err("Failed: cfg_powerdn_regs.%d\n", ret);
    }
#else
    //power down with hardware interface
    if ((ret = regulator_disable(pJpegDev->jpeg_supply)) != 0) {
        cam_err("Failed: regulator_disable.%d\n", ret);
    }
#endif

    pJpegDev->jpeg_power_ref--;
    cam_info("%s: jpeg power down, ref=%d\n", __func__, pJpegDev->jpeg_power_ref);

    return ret;
}

static void hjpeg160_do_smmu_config_cs(bool bypass)
{
    void __iomem* smmu_base_addr;
    u32 tmpVal;

    cam_info("%s enter ",__func__);

    //should ioremap once in probe function
    smmu_base_addr = ioremap(REG_BASE_SMMU, SMMU_MEM_SIZE);
    if (IS_ERR_OR_NULL(smmu_base_addr)) {
        cam_err("%s(%d) remap failed",__func__, __LINE__);
        return;
    }

    /* disable SMMU for global */
    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr + SMMU_GLOBAL_BYPASS));
    tmpVal = (tmpVal & 0xFFFFFFFE);
    REG_SET((void __iomem*)((char*)smmu_base_addr + SMMU_GLOBAL_BYPASS), tmpVal);

    if (bypass) {
        /* disable SMMU only for JPGENC stream id */
        REG_SET((void __iomem*)((char*)smmu_base_addr + SMMU_SMRX_NS_STREAM_ID_0),
            REG_GET((void __iomem*)(smmu_base_addr + SMMU_SMRX_NS_STREAM_ID_0))|0x1);
        REG_SET((void __iomem*)((char*)smmu_base_addr + SMMU_SMRX_NS_STREAM_ID_4),
            REG_GET((void __iomem*)((char*)smmu_base_addr + SMMU_SMRX_NS_STREAM_ID_4))|0x1);
        REG_SET((void __iomem*)((char*)smmu_base_addr + SMMU_SMRX_NS_STREAM_ID_5),
            REG_GET((void __iomem*)((char*)smmu_base_addr + SMMU_SMRX_NS_STREAM_ID_5))|0x1);
        goto exit;
    }

    //config smmu
    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr+SMMU_INTCLR_NS));
    cam_info("%s: get reg 0x%x = 0x%x", __func__, SMMU_INTCLR_NS, tmpVal);
    tmpVal = 0xFF;
    REG_SET((void __iomem*)((char*)smmu_base_addr+SMMU_INTCLR_NS), tmpVal);
    cam_info("%s: set reg 0x%x = 0x%x", __func__, SMMU_INTCLR_NS, tmpVal);

    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr+SMMU_INTMASK_NS));
    cam_info("%s: get reg 0x%x = 0x%x", __func__, SMMU_INTMASK_NS, tmpVal);
    tmpVal = 0x0;
    REG_SET((void __iomem*)((char*)smmu_base_addr+SMMU_INTMASK_NS), tmpVal);
    cam_info("%s: set reg 0x%x = 0x%x", __func__, SMMU_INTMASK_NS, tmpVal);

    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr+SMMU_RLD_EN0_NS));
    cam_info("%s: get reg 0x%x = 0x%x", __func__, SMMU_RLD_EN0_NS, tmpVal);
    tmpVal |= 0x31;
    REG_SET((void __iomem*)((char*)smmu_base_addr+SMMU_RLD_EN0_NS), tmpVal);
    cam_info("%s: set reg 0x%x = 0x%x", __func__, SMMU_RLD_EN0_NS, tmpVal);

    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr+SMMU_RLD_EN1_NS));
    cam_info("%s: get reg 0x%x = 0x%x", __func__, SMMU_RLD_EN1_NS, tmpVal);
    tmpVal |= 0x31;
    REG_SET((void __iomem*)((char*)smmu_base_addr+SMMU_RLD_EN1_NS), tmpVal);
    cam_info("%s: set reg 0x%x = 0x%x", __func__, SMMU_RLD_EN1_NS, tmpVal);

    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr+SMMU_RLD_EN2_NS));
    cam_info("%s: get reg 0x%x = 0x%x", __func__, SMMU_RLD_EN2_NS, tmpVal);
    tmpVal |= 0x31;
    REG_SET((void __iomem*)((char*)smmu_base_addr+SMMU_RLD_EN2_NS), tmpVal);
    cam_info("%s: set reg 0x%x = 0x%x", __func__, SMMU_RLD_EN2_NS, tmpVal);

    //SMMU Context Config
    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr+SMMU_CB_TTBR0));
    cam_info("%s: get reg 0x%x = 0x%x", __func__, SMMU_CB_TTBR0, tmpVal);
    tmpVal = s_hjpeg160.phy_pgd_base;
    REG_SET((void __iomem*)((char*)smmu_base_addr+SMMU_CB_TTBR0), tmpVal);
    cam_info("%s: set reg 0x%x = 0x%x", __func__, SMMU_CB_TTBR0, tmpVal);

    tmpVal = REG_GET((void __iomem*)((char*)smmu_base_addr+SMMU_CB_TTBCR));
    cam_info("%s: get reg 0x%x = 0x%x", __func__, SMMU_CB_TTBCR, tmpVal);
    tmpVal |= 0x1;
    REG_SET((void __iomem*)((char*)smmu_base_addr+SMMU_CB_TTBCR), tmpVal);
    cam_info("%s: set reg 0x%x = 0x%x", __func__, SMMU_CB_TTBCR, tmpVal);
exit:
    iounmap(smmu_base_addr);
}

static void hjpeg160_get_dts(struct platform_device* pDev)
{
    int ret;
    int i;
    struct device *pdev = &(pDev->dev);
    struct device_node *np = pdev->of_node;
    const char* clk_name[JPEG_CLK_MAX];
    u32 chip_type = CT_ES;

    cam_info("%s enter\n",__func__);

    if (NULL == np) {
        cam_err("%s: of node NULL.", __func__);
        return;
    }

    ret = of_property_read_u32(np, "huawei,chip_type", &chip_type);
    if (ret < 0) {
        cam_err("%s: get chip_type flag failed.", __func__);
    }
    s_hjpeg160.chip_type = chip_type;
    cam_info("%s: chip_type=%d", __func__, chip_type);

    if (chip_type == CT_ES) {
        cam_info("%s: it's ES, just return.", __func__);
        return;
    }

    //get supply for jpeg
    s_hjpeg160.jpeg_supply = devm_regulator_get(pdev, "hjpeg-srt");
    if (IS_ERR(s_hjpeg160.jpeg_supply)) {
        cam_err("[%s] Failed : ISPSRT devm_regulator_get.%p\n", __func__, s_hjpeg160.jpeg_supply);
        return;
    }

    //get clk parameters
    if ((ret = of_property_read_string_array(np, "clock-names", clk_name, JPEG_CLK_MAX)) < 0) {
        cam_err("[%s] Failed : of_property_read_string_array.%d\n", __func__, ret);
        return;
    }
    for (i = 0; i < ARRAY_SIZE(clk_name); ++i)
    {
        cam_info("[%s] clk_name[%d] = %s\n", __func__, i, clk_name[i]);
    }
    if ((ret = of_property_read_u32_array(np, "clock-value", (unsigned int*)(&(s_hjpeg160.jpegclk_value[0])), JPEG_CLK_MAX)) < 0) {
        cam_err("[%s] Failed: of_property_read_u32_array.%d\n", __func__, ret);
        return;
    }
    for (i = 0; i < JPEG_CLK_MAX; i++) {
        s_hjpeg160.jpegclk[i] = devm_clk_get(pdev, clk_name[i]);
        if (IS_ERR_OR_NULL(s_hjpeg160.jpegclk[i])) {
            cam_err("[%s] Failed : jpgclk.%s.%d.%li\n", __func__, clk_name[i], i, PTR_ERR(s_hjpeg160.jpegclk[i]));
            return;
        }
        cam_info("[%s] Jpeg clock.%d.%s: %d Hz\n", __func__, i, clk_name[i], s_hjpeg160.jpegclk_value[i]);
    }

    get_phy_pgd_base(pdev);
}

static void get_phy_pgd_base(struct device* pdev)
{
    int ret;
    struct iommu_domain_data *info;

    s_hjpeg160.phy_pgd_base = 0;
    //get iommu page
    if ((s_hjpeg160.domain = iommu_domain_alloc(pdev->bus)) == NULL) {
        pr_err("[%s] Failed : iommu_domain_alloc.%p\n", __func__, s_hjpeg160.domain);
        return;
    }
    if ((ret = iommu_attach_device(s_hjpeg160.domain, pdev)) != 0) {
        iommu_domain_free(s_hjpeg160.domain);
        pr_err("[%s] Failed : iommu_attach_device.%d\n", __func__, ret);
        return;
    }
    if ((info = (struct iommu_domain_data *)s_hjpeg160.domain->priv) == NULL) {
        iommu_detach_device(s_hjpeg160.domain, pdev);
        iommu_domain_free(s_hjpeg160.domain);
        pr_err("[%s] Failed : info.%p\n",__func__, info);
        return;
    }
    s_hjpeg160.phy_pgd_base = (unsigned int)info->phy_pgd_base;
    pr_info("[%s] info.iova.(0x%x, 0x%x) phy_pgd_base.0x%x\n", __func__,
            info->iova_start,
            info->iova_size,
            s_hjpeg160.phy_pgd_base);
    iommu_detach_device(s_hjpeg160.domain, pdev);
    iommu_domain_free(s_hjpeg160.domain);
}

static int hjpeg_need_powerup(unsigned int refs)
{
    if (refs == 0xffffffff)
        cam_err("%s:need_powerup exc, refs == 0xffffffff\n", __func__);

    return ((refs == 0) ? 1 : 0);
}

static int hjpeg_need_powerdn(unsigned int refs)
{
    if (refs == 0xffffffff)
        cam_err("%s:need_powerdn exc, refs == 0xffffffff\n", __func__);

    return ((refs == 1) ? 1 : 0);
}

static int hjpeg_setclk_enable(hjpeg160_t* pJpegDev, int idx)
{
    int ret = 0;

    cam_info("%s enter (idx=%d) \n",__func__, idx);

    if ((ret = clk_set_rate(pJpegDev->jpegclk[idx], pJpegDev->jpegclk_value[idx])) != 0) {
        cam_err("[%s] Failed: clk_set_rate.%d\n", __func__, ret);
        return ret;
    }

    if ((ret = clk_prepare_enable(pJpegDev->jpegclk[idx])) != 0) {
        cam_err("[%s] Failed: clk_prepare_enable.%d\n", __func__, ret);
        return ret;
    }

    return ret;
}

static void hjpeg_clk_disable(hjpeg160_t* pJpegDev, int idx)
{
    cam_info("%s enter (idx=%d) \n",__func__, idx);
    clk_disable_unprepare(pJpegDev->jpegclk[idx]);
}

static void set_picture_format(void __iomem* base_addr, int fmt)
{
    unsigned int tmp = 0;
    if(JPGENC_FORMAT_YUV422 == (fmt & JPGENC_FORMAT_BIT))//YUV422
    {
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_PIC_FORMAT_REG),JPGENC_ENC_PIC_FORMAT,0);
        if (JPGENC_FORMAT_UYVY == fmt)//deafult format
        {
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_Y_UV, 0);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_U_V, 0);
        }
        else if (JPGENC_FORMAT_VYUY == fmt)
        {
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_Y_UV, 0);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_U_V, 1);
        }
        else if (JPGENC_FORMAT_YVYU == fmt)
        {
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_Y_UV, 1);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_U_V, 1);
        }
        else//JPGENC_FORMAT_YUYV
        {
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_Y_UV, 1);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_U_V, 0);
        }
        REG_SET((void __iomem*)((char*)base_addr + JPGENC_INPUT_SWAP_REG),tmp);
    }
    else if(JPGENC_FORMAT_NV12 == fmt)
    {
            SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_PIC_FORMAT_REG),JPGENC_ENC_PIC_FORMAT,1);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_Y_UV, 0);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_U_V, 0);
            REG_SET((void __iomem*)((char*)base_addr + JPGENC_INPUT_SWAP_REG),tmp);
    }
    else//JPGENC_FORMAT_NV21
    {
            SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_PIC_FORMAT_REG),JPGENC_ENC_PIC_FORMAT,1);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_Y_UV, 0);
            REG_SET_FIELD(tmp,JPGENC_SWAPIN_U_V, 1);//swap U and V
            REG_SET((void __iomem*)((char*)base_addr + JPGENC_INPUT_SWAP_REG),tmp);
    }
}

static void set_picture_quality(void __iomem* base_addr, unsigned int quality)
{
    int scaler;
    unsigned int tmp;
    int length;
    uint32_t temp;
    int i;

    if(quality == 0)
    {
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TQ_Y_SELECT_REG),JPGENC_TQ0_SELECT,0);
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TQ_U_SELECT_REG),JPGENC_TQ1_SELECT,1);
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TQ_V_SELECT_REG),JPGENC_TQ2_SELECT,1);
    }
    else
    {
        if(quality < 50)
            scaler = 5000/quality;
        else
            scaler = 200 - quality*2;

        /*q-table 2*/
        length = ARRAY_SIZE(luma_qtable2);
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),JPGENC_TABLE_ID,2);
        for(i = 1;i < length;i = i + 2)
        {
            temp = (luma_qtable2[i - 1]*scaler + 50L)/100L;
            if(temp <= 0L)
                temp = 1L;
            if(temp >255L)
                temp = 255L;
            tmp = 0;
            REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,temp);
            temp = (luma_qtable2[i]*scaler + 50L)/100L;
            if(temp <= 0L)
                temp = 1L;
            if(temp >255L)
                temp = 255L;
            REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,temp);
            REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
        }

        /*q-table 3*/
        length = ARRAY_SIZE(chroma_qtable2);
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_ID_REG),JPGENC_TABLE_ID,3);
        for(i = 1;i < length;i = i + 2)
        {
            temp = (chroma_qtable2[i - 1]*scaler + 50L)/100L;
            if(temp <= 0L)
                temp = 1L;
            if(temp >255L)
                temp = 255L;
            tmp = 0;
            REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_H,temp);
            temp = (chroma_qtable2[i]*scaler + 50L)/100L;
            if(temp <= 0L)
                temp = 1L;
            if(temp >255L)
                temp = 255L;
            REG_SET_FIELD(tmp,JPGENC_TABLE_WDATA_L,temp);
            REG_SET((void __iomem*)((char*)base_addr + JPGENC_JPE_TABLE_DATA_REG),tmp);
        }

        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TQ_Y_SELECT_REG),JPGENC_TQ0_SELECT,2);
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TQ_U_SELECT_REG),JPGENC_TQ1_SELECT,3);
        SET_FIELD_TO_REG((void __iomem*)((char*)base_addr + JPGENC_JPE_TQ_V_SELECT_REG),JPGENC_TQ2_SELECT,3);
    }
}

static struct platform_driver
s_hjpeg160_driver =
{
    .driver =
    {
        .name = "huawei,hjpeg160",
        .owner = THIS_MODULE,
        .of_match_table = s_hjpeg160_dt_match,
    },
};

static int32_t hjpeg160_platform_probe(
        struct platform_device* pdev )
{
    int32_t ret;
    cam_info("%s enter\n", __func__);

    ret = hjpeg_register(pdev, &s_hjpeg160.intf);
    s_hjpeg160.pdev = pdev;
    s_hjpeg160.power_on_state = false;

    hjpeg160_get_dts(pdev);

    return ret;
}

static int __init
hjpeg160_init_module(void)
{
    cam_info("%s enter\n", __func__);
    return platform_driver_probe(&s_hjpeg160_driver,
            hjpeg160_platform_probe);
}

static void __exit
hjpeg160_exit_module(void)
{
    cam_info("%s enter\n", __func__);
    hjpeg_unregister(&s_hjpeg160.intf);
    platform_driver_unregister(&s_hjpeg160_driver);
}

module_init(hjpeg160_init_module);
module_exit(hjpeg160_exit_module);
MODULE_DESCRIPTION("hjpeg160 driver");
MODULE_LICENSE("GPL v2");
//lint -restore
