/*
 * Copyright (c) 2011-2018 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/*
 *
 * This file limProcessDisassocFrame.cc contains the code
 * for processing Disassocation Frame.
 * Author:        Chandra Modumudi
 * Date:          03/24/02
 * History:-
 * Date           Modified by    Modification Information
 * --------------------------------------------------------------------
 *
 */
#include "palTypes.h"
#include "wniApi.h"
#include "sirApi.h"
#include "aniGlobal.h"
#include "wni_cfg.h"
#include "utilsApi.h"
#include "limTypes.h"
#include "limUtils.h"
#include "limAssocUtils.h"
#include "limSecurityUtils.h"
#include "limSerDesUtils.h"
#include "limSendMessages.h"
#include "schApi.h"


/**
 * limProcessDisassocFrame
 *
 *FUNCTION:
 * This function is called by limProcessMessageQueue() upon
 * Disassociation frame reception.
 *
 *LOGIC:
 *
 *ASSUMPTIONS:
 * DPH drops packets for STA with 'valid' bit in pStaDs set to '0'.
 *
 *NOTE:
 *
 * @param  pMac - Pointer to Global MAC structure
 * @param  *pRxPacketInfo - A pointer to Rx packet info structure
 * @return None
 */
void
limProcessDisassocFrame(tpAniSirGlobal pMac, tANI_U8 *pRxPacketInfo, tpPESession psessionEntry)
{
    tANI_U8                 *pBody;
    tANI_U16                aid, reasonCode;
    tpSirMacMgmtHdr    pHdr;
    tpDphHashNode      pStaDs;
    tLimMlmDisassocInd mlmDisassocInd;

    tANI_U32            frameLen;
#ifdef WLAN_FEATURE_11W
    bool need_ind_uplayer = true;
#endif
    int8_t frame_rssi;

    pHdr = WDA_GET_RX_MAC_HEADER(pRxPacketInfo);
    pBody = WDA_GET_RX_MPDU_DATA(pRxPacketInfo);
    frameLen = WDA_GET_RX_PAYLOAD_LEN(pRxPacketInfo);
    frame_rssi = (int8_t)WDA_GET_RX_RSSI_NORMALIZED(pRxPacketInfo);

    if (limIsGroupAddr(pHdr->sa))
    {
        // Received Disassoc frame from a BC/MC address
        // Log error and ignore it
        PELOGE(limLog(pMac, LOGE,
               FL("received Disassoc frame from a BC/MC address"));)

        return;
    }

    if (limIsGroupAddr(pHdr->da) && !limIsAddrBC(pHdr->da))
    {
        // Received Disassoc frame for a MC address
        // Log error and ignore it
        PELOGE(limLog(pMac, LOGE,
               FL("received Disassoc frame for a MC address"));)

        return;
    }
    if (!lim_validate_received_frame_a1_addr(pMac, pHdr->da, psessionEntry)) {
        limLog(pMac, LOGE,
               FL("rx frame doesn't have valid a1 address, dropping it"));
        return;
    }

    if (LIM_IS_STA_ROLE(psessionEntry) &&
        ((eLIM_SME_WT_DISASSOC_STATE == psessionEntry->limSmeState) ||
         (eLIM_SME_WT_DEAUTH_STATE == psessionEntry->limSmeState))) {
        /*Every 15th deauth frame will be logged in kmsg*/
        if(!(psessionEntry->disassocmsgcnt & 0xF)) {
                limLog(pMac, LOGE,
                       FL("Already processing previously received DEAUTH/Disassoc..Dropping this.. Deauth Failed cnt %d"),
                       ++psessionEntry->disassocmsgcnt);
        } else {
            psessionEntry->disassocmsgcnt++;
        }
        return;
    }

    if (frameLen < 2) {
        PELOGE(limLog(pMac, LOGE, FL("frame len less than 2"));)
        return;
    }
    // Get reasonCode from Disassociation frame body
    reasonCode = sirReadU16(pBody);

    PELOGE(limLog(pMac, LOGE,
        FL("Received Disassoc frame for Addr: "MAC_ADDRESS_STR"(mlm state=%s previous state=%s"
        "sme state=%d previous state=%d RSSI=%d),with reason code %d [%s] from "MAC_ADDRESS_STR),
        MAC_ADDR_ARRAY(pHdr->da),
        limMlmStateStr(psessionEntry->limMlmState),  limMlmStateStr(psessionEntry->limPrevMlmState),
        psessionEntry->limSmeState, psessionEntry->limPrevSmeState,
        frame_rssi, reasonCode, limDot11ReasonStr(reasonCode),
        MAC_ADDR_ARRAY(pHdr->sa));)

#ifdef WLAN_FEATURE_11W
    /* PMF: If this session is a PMF session, then ensure that this frame was protected */
    if(psessionEntry->limRmfEnabled  && (WDA_GET_RX_DPU_FEEDBACK(pRxPacketInfo) & DPU_FEEDBACK_UNPROTECTED_ERROR))
    {
        if (psessionEntry->limMlmState == eLIM_MLM_LINK_ESTABLISHED_STATE &&
            psessionEntry->limPrevMlmState ==  eLIM_MLM_JOINED_STATE)
            need_ind_uplayer = false;

        if (need_ind_uplayer) {
            PELOGE(limLog(pMac, LOGE, FL("received an unprotected disassoc from AP"));)
            // If the frame received is unprotected, forward it to the supplicant to initiate
            // an SA query
            //send the unprotected frame indication to SME
            limSendSmeUnprotectedMgmtFrameInd(pMac, pHdr->fc.subType,
                                              (tANI_U8*)pHdr, (frameLen + sizeof(tSirMacMgmtHdr)),
                                              psessionEntry->smeSessionId, psessionEntry);
            return;
        }
    }
#endif

    /**
   * Extract 'associated' context for STA, if any.
   * This is maintained by DPH and created by LIM.
   */
     pStaDs = dphLookupHashEntry(pMac, pHdr->sa, &aid, &psessionEntry->dph.dphHashTable);

    if (pStaDs == NULL)
    {
        /**
         * Disassociating STA is not associated.
         * Log error.
         */
        PELOGE(limLog(pMac, LOGE,
           FL("received Disassoc frame from STA that does not have context "
           "reasonCode=%d, addr "MAC_ADDRESS_STR),
            reasonCode,MAC_ADDR_ARRAY(pHdr->sa));)

        return;
    }

    if (limCheckDisassocDeauthAckPending(pMac, (tANI_U8*)pHdr->sa))
    {
        PELOGE(limLog(pMac, LOGE,
                    FL("Ignore the DisAssoc received, while waiting "
                    "for ack of disassoc/deauth"));)
        limCleanUpDisassocDeauthReq(pMac,(tANI_U8*)pHdr->sa, 1);
        return;
    }

    /** If we are in the Wait for ReAssoc Rsp state */
    if (limIsReassocInProgress(pMac,psessionEntry)) {
        /** If we had received the DisAssoc from,
        *     a. the Current AP during ReAssociate to different AP in same ESS
        *     b. Unknown AP
        *   drop/ignore the DisAssoc received
        */
        if (!IS_REASSOC_BSSID(pMac,pHdr->sa,psessionEntry)) {
            PELOGE(limLog(pMac, LOGE, FL("Ignore the DisAssoc received, while "
                     "Processing ReAssoc with different/unknown AP"));)
            return;
        }
        /** If the Disassoc is received from the new AP to which we tried to ReAssociate
         *  Drop ReAssoc and Restore the Previous context( current connected AP).
         */
        if (!IS_CURRENT_BSSID(pMac, pHdr->sa,psessionEntry)) {
            PELOGW(limLog(pMac, LOGW, FL("received Disassoc from the New AP to which ReAssoc is sent "));)
            limRestorePreReassocState(pMac,
                                  eSIR_SME_REASSOC_REFUSED, reasonCode,psessionEntry);
            return;
        }
    }

    if (LIM_IS_AP_ROLE(psessionEntry) || LIM_IS_BT_AMP_AP_ROLE(psessionEntry)) {
        switch (reasonCode)
        {
            case eSIR_MAC_UNSPEC_FAILURE_REASON:
            case eSIR_MAC_DISASSOC_DUE_TO_INACTIVITY_REASON:
            case eSIR_MAC_DISASSOC_LEAVING_BSS_REASON:
            case eSIR_MAC_MIC_FAILURE_REASON:
            case eSIR_MAC_4WAY_HANDSHAKE_TIMEOUT_REASON:
            case eSIR_MAC_GR_KEY_UPDATE_TIMEOUT_REASON:
            case eSIR_MAC_RSN_IE_MISMATCH_REASON:
            case eSIR_MAC_1X_AUTH_FAILURE_REASON:
                // Valid reasonCode in received Disassociation frame
                break;

            default:
                // Invalid reasonCode in received Disassociation frame
                PELOGE(limLog(pMac, LOGE,
                       FL("received Disassoc frame with invalid reasonCode "
                       "%d from "MAC_ADDRESS_STR),
                       reasonCode, MAC_ADDR_ARRAY(pHdr->sa));)
                break;
        }
    } else if ((LIM_IS_STA_ROLE(psessionEntry) ||
              LIM_IS_BT_AMP_STA_ROLE(psessionEntry)) &&
               ((psessionEntry->limSmeState != eLIM_SME_WT_JOIN_STATE) &&
                (psessionEntry->limSmeState != eLIM_SME_WT_AUTH_STATE)  &&
                (psessionEntry->limSmeState != eLIM_SME_WT_ASSOC_STATE)  &&
                (psessionEntry->limSmeState != eLIM_SME_WT_REASSOC_STATE))) {
        switch (reasonCode)
        {
            case eSIR_MAC_UNSPEC_FAILURE_REASON:
            case eSIR_MAC_DISASSOC_DUE_TO_INACTIVITY_REASON:
            case eSIR_MAC_DISASSOC_DUE_TO_DISABILITY_REASON:
            case eSIR_MAC_CLASS2_FRAME_FROM_NON_AUTH_STA_REASON:
            case eSIR_MAC_CLASS3_FRAME_FROM_NON_ASSOC_STA_REASON:
            case eSIR_MAC_MIC_FAILURE_REASON:
            case eSIR_MAC_4WAY_HANDSHAKE_TIMEOUT_REASON:
            case eSIR_MAC_GR_KEY_UPDATE_TIMEOUT_REASON:
            case eSIR_MAC_RSN_IE_MISMATCH_REASON:
            case eSIR_MAC_1X_AUTH_FAILURE_REASON:
            case eSIR_MAC_PREV_AUTH_NOT_VALID_REASON:
            case eSIR_MAC_PEER_REJECT_MECHANISIM_REASON:
            case eSIR_MAC_XS_UNACKED_FRAMES_REASON:
                // Valid reasonCode in received Disassociation frame
                break;

            case eSIR_MAC_DEAUTH_LEAVING_BSS_REASON:
            case eSIR_MAC_DISASSOC_LEAVING_BSS_REASON:
                // Valid reasonCode in received Disassociation frame
                // as long as we're not about to channel switch
                if(psessionEntry->gLimChannelSwitch.state != eLIM_CHANNEL_SWITCH_IDLE)
                {
                    limLog(pMac, LOGE,
                        FL("Ignoring disassoc frame due to upcoming "
                           "channel switch, from "MAC_ADDRESS_STR),MAC_ADDR_ARRAY(pHdr->sa));
                    return;
                }
                break;

            default:
                break;
        }
    }
    else
    {
        // Received Disassociation frame in either IBSS
        // or un-known role. Log and ignore it
        limLog(pMac, LOGE,
               FL("received Disassoc frame with invalid reasonCode %d in role "
               "%d in sme state %d from "MAC_ADDRESS_STR), reasonCode,
               GET_LIM_SYSTEM_ROLE(psessionEntry), psessionEntry->limSmeState,
               MAC_ADDR_ARRAY(pHdr->sa));

        return;
    }

    if ((pStaDs->mlmStaContext.mlmState == eLIM_MLM_WT_DEL_STA_RSP_STATE) ||
        (pStaDs->mlmStaContext.mlmState == eLIM_MLM_WT_DEL_BSS_RSP_STATE)) {
        /**
         * Already in the process of deleting context for the peer
         * and received Disassociation frame. Log and Ignore.
         */
        PELOGE(limLog(pMac, LOGE,
               FL("received Disassoc frame in state %d from "MAC_ADDRESS_STR
               ",isDisassocDeauthInProgress : %d\n"),
               pStaDs->mlmStaContext.mlmState, MAC_ADDR_ARRAY(pHdr->sa),
               pStaDs->isDisassocDeauthInProgress);)

        return;
    }

#ifdef FEATURE_WLAN_TDLS
    /* Delete all the TDLS peers only if Disassoc is received from the AP */
    if ((LIM_IS_STA_ROLE(psessionEntry)) &&
        ((pStaDs->mlmStaContext.mlmState == eLIM_MLM_LINK_ESTABLISHED_STATE) ||
        (pStaDs->mlmStaContext.mlmState == eLIM_MLM_IDLE_STATE)) &&
        (IS_CURRENT_BSSID(pMac, pHdr->sa, psessionEntry)))
        limDeleteTDLSPeers(pMac, psessionEntry);
#endif

    if (pStaDs->mlmStaContext.mlmState != eLIM_MLM_LINK_ESTABLISHED_STATE)
    {
        /**
         * Requesting STA is in some 'transient' state?
         * Log error.
         */
        if (pStaDs->mlmStaContext.mlmState == eLIM_MLM_WT_ASSOC_CNF_STATE)
            pStaDs->mlmStaContext.updateContext = 1;

        PELOGE(limLog(pMac, LOGE,
               FL("received Disassoc frame from peer that is in state %X, addr "
               MAC_ADDRESS_STR),
               pStaDs->mlmStaContext.mlmState, MAC_ADDR_ARRAY(pHdr->sa));)

    } // if (pStaDs->mlmStaContext.mlmState != eLIM_MLM_LINK_ESTABLISHED_STATE)

    pStaDs->mlmStaContext.cleanupTrigger = eLIM_PEER_ENTITY_DISASSOC;
    pStaDs->mlmStaContext.disassocReason = (tSirMacReasonCodes) reasonCode;

    // Issue Disassoc Indication to SME.
    vos_mem_copy((tANI_U8 *) &mlmDisassocInd.peerMacAddr,
                 (tANI_U8 *) pStaDs->staAddr,
                 sizeof(tSirMacAddr));
    mlmDisassocInd.reasonCode =
        (tANI_U8) pStaDs->mlmStaContext.disassocReason;
    mlmDisassocInd.disassocTrigger = eLIM_PEER_ENTITY_DISASSOC;

    /* Update PE session Id  */
    mlmDisassocInd.sessionId = psessionEntry->peSessionId;

    /*
     * reset the deauthMsgCnt here since we are able to Process
     * the deauth frame and sending up the indication as well
     */
     if (psessionEntry->disassocmsgcnt != 0)
         psessionEntry->disassocmsgcnt = 0;

    if (limIsReassocInProgress(pMac,psessionEntry)) {

    /* If we're in the middle of ReAssoc and received disassoc from
     * the ReAssoc AP, then notify SME by sending REASSOC_RSP with
     * failure result code. By design, SME will then issue "Disassoc"
     * and cleanup will happen at that time.
     */
        PELOGE(limLog(pMac, LOGE, FL("received Disassoc from AP while waiting "
                                  "for Reassoc Rsp"));)

        if (psessionEntry->limAssocResponseData) {
            vos_mem_free(psessionEntry->limAssocResponseData);
            psessionEntry->limAssocResponseData = NULL;
        }

        limRestorePreReassocState(pMac,eSIR_SME_REASSOC_REFUSED, reasonCode,psessionEntry);
        return;
    }

    lim_update_lost_link_info(pMac, psessionEntry, frame_rssi);
    limPostSmeMessage(pMac, LIM_MLM_DISASSOC_IND,
                      (tANI_U32 *) &mlmDisassocInd);


    // send eWNI_SME_DISASSOC_IND to SME
    limSendSmeDisassocInd(pMac, pStaDs,psessionEntry);

    return;
} /*** end limProcessDisassocFrame() ***/
