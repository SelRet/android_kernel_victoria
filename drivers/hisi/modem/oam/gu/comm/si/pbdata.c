/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2012-2015. All rights reserved.
 * foss@huawei.com
 *
 * If distributed as part of the Linux kernel, the following license terms
 * apply:
 *
 * * This program is free software; you can redistribute it and/or modify
 * * it under the terms of the GNU General Public License version 2 and
 * * only version 2 as published by the Free Software Foundation.
 * *
 * * This program is distributed in the hope that it will be useful,
 * * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * * GNU General Public License for more details.
 * *
 * * You should have received a copy of the GNU General Public License
 * * along with this program; if not, write to the Free Software
 * * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
 *
 * Otherwise, the following license terms apply:
 *
 * * Redistribution and use in source and binary forms, with or without
 * * modification, are permitted provided that the following conditions
 * * are met:
 * * 1) Redistributions of source code must retain the above copyright
 * *    notice, this list of conditions and the following disclaimer.
 * * 2) Redistributions in binary form must reproduce the above copyright
 * *    notice, this list of conditions and the following disclaimer in the
 * *    documentation and/or other materials provided with the distribution.
 * * 3) Neither the name of Huawei nor the names of its contributors may
 * *    be used to endorse or promote products derived from this software
 * *    without specific prior written permission.
 *
 * * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/************************************************************************
  Copyright    : 2005-2007, Huawei Tech. Co., Ltd.
  File name    : UsimmApi.c
  Author       : zhuli
  Version      : V100R002
  Date         : 2008-5-15
  Description  : 该C文件给出了---完成缓冲内容处理模块实现
  Function List:
  History      :
 ************************************************************************/

#include "si_pb.h"
#include "TafTypeDef.h"
#include "UsimPsInterface.h"
#include "NVIM_Interface.h"

/*****************************************************************************
    协议栈打印打点方式下的.C文件宏定义
*****************************************************************************/
#define    THIS_FILE_ID PS_FILE_ID_PB_DATA_C


/*****************************************************************************
函 数 名  :SI_PB_FindPBOffset

功能描述  :用于在缓冲中定位电话本在列表中的偏移

输入参数  :enPBType:输入查询的电话本类型

输出参数  :pucOffset:输出查询的电话本偏移

返 回 值  :VOS_OK:查询成功
           VOS_ERR:查询失败

调用函数  :无

被调函数  :

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_UINT32 SI_PB_FindPBOffset(SI_PB_TYPE_ENUM_UINT32  enPBType, VOS_UINT8 *pucOffset)
{
    VOS_UINT8 i;

    for(i=0; i<SI_PB_MAX_NUMBER; i++)
    {
        if(gastPBContent[i].enPBType == enPBType)   /*当前的查询类型一致*/
        {
            PB_INFO_LOG("SI_PB_FindPBOffset Info: Locate the PhoneBook Accurately");

            *pucOffset = i;

            return VOS_OK;
        }
    }

    PB_ERROR_LOG("SI_PB_FindPBOffset Error: The PhoneBook Info is Not Exist");

    return VOS_ERR;             /*当前未找到偏移*/
}


/*****************************************************************************
函 数 名  :SI_PB_LocateRecord

功能描述  :定位当前访问的电话本在缓冲中的位置

输入参数  :enPBType:输入查询的电话本类型
           usIndex1:访问的电话本的范围起始地址
           usIndex2:访问的电话本的范围结束地址

输出参数  :pucNumber:电话本缓冲在列表中的偏移
           pucContent:电话本访问内容的起实地址

返 回 值  :VOS_OK:查询成功
           VOS_ERR:查询失败

调用函数  :SI_PB_FindPBOffset

被调函数  :

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_UINT32 SI_PB_LocateRecord(SI_PB_TYPE_ENUM_UINT32  enPBType, VOS_UINT16 usIndex1, VOS_UINT16 usIndex2, VOS_UINT8 *pucNumber)
{
    VOS_UINT8   ucPBOffset;
    VOS_UINT32 ulResult;

    ulResult = SI_PB_FindPBOffset(enPBType, &ucPBOffset);    /*首先查询当前的缓冲位置*/

    if(VOS_ERR == ulResult)
    {
        PB_WARNING_LOG("SI_PB_LocateRecord: Locate PhoneBook Error");

        return TAF_ERR_UNSPECIFIED_ERROR;
    }

    if(PB_NOT_INITIALISED == gastPBContent[ucPBOffset].enInitialState)   /*当前的电话本没有初始化完毕*/
    {
        PB_ERROR_LOG("SI_PB_LocateRecord:The PhoneBook is Not Initializtion");

        return TAF_ERR_SIM_BUSY;
    }

    if(PB_FILE_NOT_EXIST == gastPBContent[ucPBOffset].enInitialState)   /*当前的电话本没有初始化完毕*/
    {
        PB_ERROR_LOG("SI_PB_LocateRecord:The PhoneBook is Not Initializtion");

        return TAF_ERR_FILE_NOT_EXIST;
    }

    if((usIndex1 > gastPBContent[ucPBOffset].usTotalNum)
        || (usIndex2 > gastPBContent[ucPBOffset].usTotalNum)
        || (usIndex1 > usIndex2))/*当前的索引已经超出范围*/
    {
        PB_WARNING_LOG("SI_PB_LocateRecord: The Index is Not in The Range of PhoneBook");

        return TAF_ERR_PB_WRONG_INDEX;
    }

    *pucNumber = ucPBOffset;        /*返回当前的电话本缓冲偏移*/

    return VOS_OK;
}

/*****************************************************************************
函 数 名  :SI_PB_CountADNRecordNum

功能描述  :计算缓冲索引号对应的电话本文件的ID和记录号

输入参数  :usIndex:索引号

输出参数  :pusFileId:对应的电话本文件
           pucRecordNum:对应的电话本记录号

返 回 值  :VOS_OK:查询成功
           VOS_ERR:查询失败

调用函数  :无

被调函数  :

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_UINT32 SI_PB_CountADNRecordNum(VOS_UINT16 usIndex, VOS_UINT16 *pusFileId, VOS_UINT8 *pucRecordNum)
{
    VOS_UINT8 i;
    VOS_UINT16 usTemp = 0;

    for(i=0; i<SI_PB_ADNMAX; i++)   /*根据当前的ADN列表内容循环*/
    {
        if((usTemp < usIndex)&&(usIndex <= (gstPBCtrlInfo.astADNInfo[i].ucRecordNum + usTemp)))/*索引号在文件的范围内*/
        {
            *pusFileId = gstPBCtrlInfo.astADNInfo[i].usFileID;  /*返回当前的文件ID*/
            *pucRecordNum = (VOS_UINT8)(usIndex - usTemp);/*返回当前的记录号*/

            return VOS_OK;
        }
        else
        {
            usTemp += gstPBCtrlInfo.astADNInfo[i].ucRecordNum;  /*索引号累计*/
        }
    }

    return VOS_ERR;
}

/*****************************************************************************
函 数 名  : SI_PB_GetXDNFileID
功能描述  : 根据XDN电话本类型返回对应电话本的FID
输入参数  : ulStorage:XDN点话本类型
输出参数  : pusFileId:电话本对应的FID
返 回 值  : VOS_OK
            VOS_ERR
调用函数  :
被调函数  :
修订记录  :
1.  日    期   : 2008年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_UINT32 SI_PB_GetXDNFileID(VOS_UINT32 ulStorage, VOS_UINT16 *pusFileId)
{
    VOS_UINT32 ulResult = VOS_OK;

    switch(ulStorage)
    {
        case SI_PB_STORAGE_ON:
            *pusFileId = EFMSISDN;
            break;
        case SI_PB_STORAGE_FD:
            *pusFileId = EFFDN;
            break;
        case SI_PB_STORAGE_BD:
            *pusFileId = EFBDN;
            break;
        default:
            ulResult = VOS_ERR;
    }

    return ulResult;
}

/*****************************************************************************
函 数 名  :SI_PB_BcdToAscii

功能描述  :BCD码转换成Ascii码

输入参数  :ucBcdNumLen:转换的号码长度
           pucBcdNum:转换的号码内容

输出参数  :pucAsciiNum:转换的结果

返 回 值  :无

调用函数  :无

被调函数  :

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat
2. 日    期   : 2011年08月04日
    作    者   : w00184875
    修改内容   : DTS2011080203533

*****************************************************************************/
VOS_VOID SI_PB_BcdToAscii(VOS_UINT8 ucBcdNumLen,VOS_UINT8 *pucBcdNum, VOS_UINT8 *pucAsciiNum, VOS_UINT8 *pucLen)
{

    VOS_UINT8       ucTmp;
    VOS_UINT8       ucLen = 0;
    VOS_UINT8       ucFirstNumber;
    VOS_UINT8       ucSecondNumber;

    for (ucTmp = 0; ucTmp < ucBcdNumLen; ucTmp++)
    {

        if(0xFF == pucBcdNum[ucTmp])
        {
            break;
        }

        ucFirstNumber  = (VOS_UINT8)(pucBcdNum[ucTmp] & 0x0F); /*取出高半字节*/

        ucSecondNumber = (VOS_UINT8)((pucBcdNum[ucTmp] >> 4) & 0x0F);/*取出低半字节*/

        if(ucFirstNumber <= 9)  /*转换数字*/
        {
            *pucAsciiNum = ucFirstNumber + 0x30;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0A == ucFirstNumber)   /*转换*字符*/
        {
            *pucAsciiNum = 0x2a;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0B == ucFirstNumber)   /*转换#字符*/
        {
            *pucAsciiNum = 0x23;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0C == ucFirstNumber)   /*转换'P'字符*/
        {
            *pucAsciiNum = 0x50;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0D == ucFirstNumber)   /*转换'?'字符*/
        {
            *pucAsciiNum = 0x3F;

            pucAsciiNum++;

            ucLen++;
        }
        else                                    /*转换字母*/
        {
            *pucAsciiNum = ucFirstNumber + 0x57;

            pucAsciiNum++;

            ucLen++;
        }

        if(0x0F == ucSecondNumber)
        {
            break;
        }
        if(ucSecondNumber <= 9)
        {
            *pucAsciiNum = ucSecondNumber + 0x30;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0A == ucSecondNumber)
        {
            *pucAsciiNum = 0x2a;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0B == ucSecondNumber)
        {
            *pucAsciiNum = 0x23;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0C == ucSecondNumber)   /*转换'P'字符*/
        {
            *pucAsciiNum = 0x50;

            pucAsciiNum++;

            ucLen++;
        }
        else if(0x0D == ucSecondNumber)   /*转换'?'字符*/
        {
            *pucAsciiNum = 0x3F;

            pucAsciiNum++;

            ucLen++;
        }
        else
        {
            *pucAsciiNum = ucSecondNumber + 0x57;

            pucAsciiNum++;

            ucLen++;
        }
    }

    *pucLen = ucLen;

    return;
}

/*****************************************************************************
函 数 名  :SI_PB_TransPBName

功能描述  :解析电话本姓名

输入参数  :ucNameMax 姓名长度的最大值
           pucName 姓名内容

输出参数  :pAlphaType 转换的姓名编码类型
           pNameLen 转换的姓名长度

返 回 值  :VOS_OK 解析成功
           VOS_ERR 解析失败

调用函数  :无

被调函数  :

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_VOID SI_PB_DecodePBName(VOS_UINT8 ucNameMax, VOS_UINT8 *pucName,
                                        VOS_UINT32 *pAlphaType, VOS_UINT8 *pNameLen)
{
    VOS_UINT8 i = 0;

    /* 姓名是UCS2 80 */
    if (SI_PB_ALPHATAG_TYPE_UCS2_80 == pucName[0])
    {
        *pAlphaType = pucName[0];

        ucNameMax--;
        for(i=0;i<(ucNameMax-(ucNameMax%2));i+=2)   /*遍历当前的姓名长度*/
        {
            if((pucName[i+1] == 0xFF)&&(pucName[i+2] == 0xFF))
            {
                break;
            }
        }

        *pNameLen = i;      /*姓名长度返回*/

        return;
    }

    /* 姓名是UCS2 81 */
    if (SI_PB_ALPHATAG_TYPE_UCS2_81 == pucName[0])
    {
        *pAlphaType = pucName[0];

        if(pucName[1] > (ucNameMax - 3))/* 如果当前长度信息超过最大长度，则直接阶截断 */
        {
           i = ucNameMax -1;
        }
        else
        {
           i = pucName[1] + 2;     /*计算当前姓名长度*/
        }

        *pNameLen = i;      /*姓名长度返回*/

        return;
    }

    /* 姓名是UCS2 82 */
    if (SI_PB_ALPHATAG_TYPE_UCS2_82 == pucName[0])
    {
        *pAlphaType = pucName[0];

        if(pucName[1] > (ucNameMax - 4))/* 如果当前长度信息超过最大长度，则直接阶截断 */
        {
           i = ucNameMax -1;
        }
        else
        {
           i = pucName[1] + 3;     /*计算当前姓名长度*/
        }

        *pNameLen = i;      /*姓名长度返回*/

        return;
    }


    /* 姓名是GSM格式存储 */
    *pAlphaType = SI_PB_ALPHATAG_TYPE_GSM;

    for (i = 0; i < ucNameMax; i++)         /*遍历当前的姓名长度*/
    {
        if (0xFF == pucName[i])
        {
            break;
        }
    }

    *pNameLen = i;      /*姓名长度返回*/

    return;
}

/*****************************************************************************
函 数 名  :SI_PB_TransPBFromate

功能描述  :转换当前的一条电话本内容为指定的数据结构

输入参数  :pstPBContent 转换的电话本内容信息
           ucIndex 转换的电话本的索引号/记录号
           pContent 转换的电话本内容

输出参数  :pstRecord 转换的电话本数据结构

返 回 值  :VOS_OK 解析成功
           VOS_ERR 解析失败

调用函数  :
被调函数  :
修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_VOID SI_PB_TransPBFromate(SI_PB_CONTENT_STRU *pstPBContent, VOS_UINT16 usIndex, VOS_UINT8 *pContent, SI_PB_RECORD_STRU *pstRecord)
{
    VOS_UINT32 ulResult;
    VOS_UINT32 ulExtInfoNum;
    VOS_UINT8  ucExtRecord;
    VOS_UINT8  aucPhoneNumber[20] = {0};
    VOS_UINT8  *pucExtContent;
    VOS_UINT8  ucExtNumLen;
    VOS_UINT8  ucXdnNumLen;

    ulResult = SI_PB_CheckContentValidity(pstPBContent, pContent);/*检查当前的内容是否有效*/

    if(ulResult != VOS_OK)
    {
        pstRecord->ValidFlag = SI_PB_CONTENT_INVALID;     /*标记当前的内容无效*/

        /*全置为0*/
    }
    else
    {
        pstRecord->ValidFlag = SI_PB_CONTENT_VALID;/*标记当前的内容有效*/

        SI_PB_DecodePBName(pstPBContent->ucNameLen, pContent,
                            &pstRecord->AlphaTagType,&pstRecord->ucAlphaTagLength);

        if(pstRecord->ucAlphaTagLength != 0x00)         /*当前姓名不为空*/
        {
            if(pstRecord->AlphaTagType == SI_PB_ALPHATAG_TYPE_GSM)  /*拷贝英文姓名从头开始*/
            {
                PAM_MEM_CPY_S(pstRecord->AlphaTag, pstRecord->ucAlphaTagLength, pContent, pstRecord->ucAlphaTagLength);
            }
            else                                                                                        /*拷贝中文姓名从第二个字节开始*/
            {
                /*ucs2编码，拷贝从长度字段开始*/
                PAM_MEM_CPY_S(pstRecord->AlphaTag, pstRecord->ucAlphaTagLength, pContent+1, pstRecord->ucAlphaTagLength);
            }
        }

        if ((pContent[pstPBContent->ucNameLen+1] == 0xFF)
            && (pContent[pstPBContent->ucNameLen] == 0x0))
        {
            pstRecord->NumberType   =  PB_NUMBER_TYPE_NORMAL;
        }
        else
        {
            pstRecord->NumberType   =  pContent[pstPBContent->ucNameLen+1];
        }

        ucExtRecord = ((PB_BDN == pstPBContent->enPBType)?\
                        pContent[pstPBContent->ucRecordLen-2]:\
                        pContent[pstPBContent->ucRecordLen-1]);

        ulExtInfoNum  = pstPBContent->ulExtInfoNum;
        pucExtContent = gastEXTContent[ulExtInfoNum].pExtContent;

        ucExtRecord = ((VOS_NULL_PTR == pucExtContent) ? 0xFF : (ucExtRecord));

        if(pContent[pstPBContent->ucNameLen] < 2)           /*当前号码为空*/
        {
            pstRecord->NumberLength = 0;
        }
        else if((0xFF != ucExtRecord)&&(0 != ucExtRecord)&&(VOS_NULL_PTR != pucExtContent)
                &&(ucExtRecord <= gastEXTContent[ulExtInfoNum].usExtTotalNum))
        {
            pucExtContent += (ucExtRecord - 1) * SI_PB_EXT_LEN;
            ucExtNumLen   = (*(pucExtContent + 1) > (SI_PB_NUM_LEN/2))?
                            (SI_PB_NUM_LEN/2) : *(pucExtContent + 1);

            PAM_MEM_CPY_S(aucPhoneNumber, SI_PB_NUM_LEN/2, pContent+(pstPBContent->ucNameLen+2), SI_PB_NUM_LEN/2 );
            PAM_MEM_CPY_S(aucPhoneNumber + (SI_PB_NUM_LEN/2), SI_PB_NUM_LEN/2, pucExtContent + 2, SI_PB_NUM_LEN/2 );

            SI_PB_BcdToAscii((VOS_UINT8)(ucExtNumLen + (SI_PB_NUM_LEN/2)), aucPhoneNumber,
                                pstRecord->Number, &pstRecord->NumberLength);
        }
        else                                                                                /*拷贝号码从内容开始*/
        {
            ucXdnNumLen = ((pContent[pstPBContent->ucNameLen]-1) > (SI_PB_NUM_LEN/2))?
                           (SI_PB_NUM_LEN/2) : (pContent[pstPBContent->ucNameLen]-1);

            SI_PB_BcdToAscii(ucXdnNumLen, &pContent[pstPBContent->ucNameLen+2],
                            pstRecord->Number, &pstRecord->NumberLength);
        }
    }

    pstRecord->Index = usIndex;

    return ;
}

/*****************************************************************************
函 数 名  :SI_PB_MemSet

功能描述  :刷新内存为FF

输入参数  :ucLen: 刷新的内存长度
           pucMem:刷新的内存地址

输出参数  :无

返 回 值  :无

调用函数  :无

被调函数  :无

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_VOID SI_PB_MemSet(VOS_UINT8 ucLen, VOS_UINT8 ucValue,VOS_UINT8 *pucMem)
{
    VOS_UINT8 i;

    for(i=0; i<ucLen; i++)
    {
        pucMem[i] = ucValue;
    }

    return;
}

/*****************************************************************************
函 数 名  :SI_PB_GetBitFromBuf
功能描述  :从数据中获取指定Bit位的值　
输入参数  :ucDataBuf:   数据内容
           ulBitNo:     Bit号，从1开始
输出参数  :无
返 回 值  : 1:有效、0:无效
调用函数  :无
修订记录  :
1. 日    期   : 2009年5月19日
    作    者   : m00128685
    修改内容   : Creat
*****************************************************************************/
VOS_UINT32 SI_PB_GetBitFromBuf(VOS_UINT8 *pucDataBuf, VOS_UINT32 ulBitNo)
{
    VOS_UINT32  ulOffset;
    VOS_UINT8   ucBit;

    if(VOS_NULL_PTR == pucDataBuf)
    {
        PB_ERROR_LOG("SI_PB_GetBitFromBuf: Input Null Ptr");

        return VOS_FALSE;
    }

    ulOffset = (ulBitNo - 1)/ 8;

    ucBit    = (VOS_UINT8)((ulBitNo - 1)%8);

    return (pucDataBuf[ulOffset]>>ucBit)&0x1;
}

/*****************************************************************************
函 数 名  :SI_PB_CheckEccValidity
功能描述  :检查ECC内容是否为空
输入参数  :pstPBContent:当前电话本的基本信息
           pContent:电话的内容
输出参数  :无
返 回 值  :VOS_OK:查询成功
           VOS_ERR:查询失败
调用函数  :无
被调函数  :
修订记录  :
1. 日    期   : 2009年12月5日
   作    者   : z00100318
   修改内容   : Creat
*****************************************************************************/
VOS_UINT32 SI_PB_CheckEccValidity(VOS_UINT8 *pContent)
{
    if (VOS_NULL_PTR == pContent)
    {
        PB_ERROR_LOG("SI_PB_CheckEccValidity: Input NULL PTR");

        return VOS_ERR;
    }

    if( 0xFF == pContent[0])
    {
        PB_INFO_LOG("SI_PB_CheckEccValidity: The ECC is Empty");

        return VOS_ERR;
    }
    else
    {
        PB_INFO_LOG("SI_PB_CheckEccValidity: The ECC is Not Empty");

        return VOS_OK;
    }
}

/*****************************************************************************
函 数 名  :SI_PB_CheckContentValidity

功能描述  :简单检查当前的电话本内容是否为空

输入参数  :pstPBContent:当前电话本的基本信息
           pContent:电话的内容

输出参数  :无

返 回 值  :VOS_OK:查询成功
           VOS_ERR:查询失败

调用函数  :无

被调函数  :

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : z00100318
    修改内容   : Creat

*****************************************************************************/
VOS_UINT32 SI_PB_CheckContentValidity(SI_PB_CONTENT_STRU *pstPBContent, VOS_UINT8 *pContent)
{
    if ((VOS_NULL_PTR == pContent)||(VOS_NULL_PTR == pstPBContent))
    {
        PB_ERROR_LOG("SI_PB_CheckContentValidity: Input NULL PTR");

        return VOS_ERR;
    }

    if(((pContent[pstPBContent->ucNameLen] == 0)||(pContent[pstPBContent->ucNameLen] == 0xFF))
        &&(pContent[0] == 0xFF))/*检查姓名和号码是否为空*/
    {
        PB_INFO_LOG("SI_PB_CheckContentValidity: The PhoneBook Content is Empty");

        return VOS_ERR;
    }
    else
    {
        PB_INFO_LOG("SI_PB_CheckContentValidity: The PhoneBook Content is Not Empty");

        return VOS_OK;
    }
}

/*****************************************************************************
函 数 名  :SI_PB_CheckANRValidity

功能描述  :简单检查当前的电话本内容是否为空

输入参数  :pstPBContent:当前电话本的基本信息
           pContent:电话的内容

输出参数  :无

返 回 值  :VOS_OK:查询成功
           VOS_ERR:查询失败

调用函数  :无

被调函数  :

修订记录  :
1. 日    期   : 2007年10月15日
    作    者   : m00128685
    修改内容   : Creat

*****************************************************************************/
VOS_UINT32 SI_PB_CheckANRValidity(VOS_UINT8 *pContent)
{
    if (VOS_NULL_PTR == pContent)
    {
        PB_ERROR_LOG("SI_PB_CheckANRValidity: Input NULL PTR");

        return VOS_ERR;
    }

    if((0xFF == pContent[0])||(0xFF == pContent[1])||(0 == pContent[1]))/*检查号码是否为空*/
    {
        PB_INFO_LOG("SI_PB_CheckANRValidity: The PhoneBook Content is Empty");

        return VOS_ERR;
    }
    else
    {
        PB_INFO_LOG("SI_PB_CheckANRValidity: The PhoneBook Content is Not Empty");

        return VOS_OK;
    }
}



/*****************************************************************************
函 数 名  :SI_PB_TransANRFromate

功能描述  :转换当前的一条ANR电话本为指定的数据结构

输入参数  :pstPBContent 转换的电话本内容信息
           ucIndex 转换的电话本的索引号/记录号
           pContent 转换的电话本内容

输出参数  :pstRecord 转换的电话本数据结构

返 回 值  :VOS_OK 解析成功
           VOS_ERR 解析失败

调用函数  :
被调函数  :
修订记录  :
1. 日    期   : 2009年06月08日
    作    者   : m00128685
    修改内容   : Creat

*****************************************************************************/
VOS_VOID SI_PB_TransANRFromate(VOS_UINT8 ucANROffset,VOS_UINT8 *pANRContent, SI_PB_RECORD_STRU *pstRecord)
{
    VOS_UINT32 ulResult;
    VOS_UINT8  ucExtRecord;
    VOS_UINT8  aucPhoneNumber[20] = {0};
    VOS_UINT8  *pucExtContent;
    VOS_UINT8  ucExtNumLen;
    VOS_UINT8  ucAnrNumLen;

    ulResult = SI_PB_CheckANRValidity(pANRContent);/*检查当前的内容是否有效*/

    if(ulResult != VOS_OK)
    {
        /*全置为0*/
        pstRecord->AdditionNumber[ucANROffset].NumberLength = 0;
    }
    else
    {
        pstRecord->ValidFlag = SI_PB_CONTENT_VALID;/*标记当前的内容有效*/

        pstRecord->AdditionNumber[ucANROffset].NumberType = pANRContent[2];

        pucExtContent = gastEXTContent[PB_ADN_CONTENT].pExtContent;

        ucExtRecord = ((VOS_NULL_PTR == pucExtContent)?0xFF:pANRContent[14]);

        if((0xFF != ucExtRecord)&&(VOS_NULL_PTR != pucExtContent)
           &&(ucExtRecord <= gastEXTContent[PB_ADN_CONTENT].usExtTotalNum))
        {
            pucExtContent += (ucExtRecord - 1) * SI_PB_EXT_LEN;
            ucExtNumLen   = (*(pucExtContent + 1) > (SI_PB_NUM_LEN/2))?
                            (SI_PB_NUM_LEN/2) : *(pucExtContent + 1);

            PAM_MEM_CPY_S(aucPhoneNumber, SI_PB_NUM_LEN/2, pANRContent+3, SI_PB_NUM_LEN/2 );
            PAM_MEM_CPY_S(aucPhoneNumber + (SI_PB_NUM_LEN/2), SI_PB_NUM_LEN/2, pucExtContent + 2, SI_PB_NUM_LEN/2 );

            SI_PB_BcdToAscii( (VOS_UINT8)(ucExtNumLen + (SI_PB_NUM_LEN/2)), aucPhoneNumber,
                                pstRecord->AdditionNumber[ucANROffset].Number, &pstRecord->AdditionNumber[ucANROffset].NumberLength);
        }
        else                                                                                /*拷贝号码从内容开始*/
        {
            ucAnrNumLen = ((pANRContent[1]-1) > (SI_PB_NUM_LEN/2))?
                          (SI_PB_NUM_LEN/2) : (pANRContent[1]-1);

            SI_PB_BcdToAscii(ucAnrNumLen, &pANRContent[3],
                            pstRecord->AdditionNumber[ucANROffset].Number,
                            &pstRecord->AdditionNumber[ucANROffset].NumberLength);
        }
    }

    return;
}

/*****************************************************************************
函 数 名  :SI_PB_TransEMLFromate

功能描述  :转换Email文件内容为指定的数据结构

输入参数  :pstPBContent 转换的电话本内容信息
           ucIndex 转换的电话本的索引号/记录号
           pContent 转换的电话本内容

输出参数  :pstRecord 转换的电话本数据结构

返 回 值  :VOS_OK 解析成功
           VOS_ERR 解析失败

调用函数  :
被调函数  :
修订记录  :
1.  日    期   : 2009年06月08日
    作    者   : m00128685
    修改内容   : Creat

*****************************************************************************/
VOS_VOID SI_PB_TransEMLFromate(VOS_UINT8 ucEmailMaxLen, VOS_UINT8 *pEMLContent, SI_PB_RECORD_STRU *pstRecord)
{
    VOS_UINT32 i;
    VOS_UINT8  ucEmailLen = 0;

    for(i = 0; i < ucEmailMaxLen; i++)
    {
        if(0xFF == pEMLContent[i])
        {
            break;
        }

        ucEmailLen++;
    }

    if(0 == ucEmailLen)
    {
        pstRecord->Email.EmailLen = 0;
    }
    else
    {
        pstRecord->Email.EmailLen = ucEmailLen;

        pstRecord->ValidFlag = SI_PB_CONTENT_VALID;

        PAM_MEM_CPY_S(pstRecord->Email.Email, ucEmailLen, pEMLContent, ucEmailLen);
    }

    return;
}

/*****************************************************************************
函 数 名  :SI_PB_GetFileCntFromIndex
功能描述  :根据输入的INDEX的值得到对应的EMAIL或ANR文件的偏移，从1开始　
输入参数  :ucIndex:   INDEX的值
           pucEMLFileCnt:     返回EMAIL文件的偏移
输出参数  :无
返 回 值  : 1:有效、0:无效
调用函数  :无
修订记录  :
1. 日    期   : 2009年5月19日
    作    者   : m00128685
    修改内容   : Creat
*****************************************************************************/
VOS_UINT32 SI_PB_GetFileCntFromIndex(VOS_UINT16 ucIndex, VOS_UINT8 *pucFileCnt)
{
    VOS_UINT8                           i;
    VOS_UINT16                          usSum;

    usSum = 0;

    if(SI_PB_ADNMAX < gstPBCtrlInfo.ulADNFileNum)
    {
        PB_WARNING1_LOG("SI_PB_SearchResultProc: gstPBCtrlInfo.ulADNFileNum err", gstPBCtrlInfo.ulADNFileNum);
        return VOS_ERR;
    }

    for (i = 0; i < gstPBCtrlInfo.ulADNFileNum; i++)
    {
        if ((ucIndex <= (usSum + gstPBCtrlInfo.astADNInfo[i].ucRecordNum))
            && (ucIndex > usSum))
        {
            break;
        }
        else
        {
            usSum += gstPBCtrlInfo.astADNInfo[i].ucRecordNum;
        }
    }

    if (i >= gstPBCtrlInfo.ulADNFileNum)
    {
        *pucFileCnt = 0;
        return VOS_ERR;
    }
    else
    {
        *pucFileCnt = i + 1;
        return VOS_OK;
    }
}





