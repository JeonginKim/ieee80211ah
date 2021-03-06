/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2006, 2009 INRIA
 * Copyright (c) 2009 MIRKO BANCHI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 *          Mirko Banchi <mk.banchi@gmail.com>
 */

#include "sta-wifi-mac.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"
#include "ns3/trace-source-accessor.h"
#include "qos-tag.h"
#include "mac-low.h"
#include "dcf-manager.h"
#include "mac-rx-middle.h"
#include "mac-tx-middle.h"
#include "wifi-mac-header.h"
#include "extension-headers.h"
#include "msdu-aggregator.h"
#include "amsdu-subframe-header.h"
#include "mgt-headers.h"
#include "ht-capabilities.h"
#include "random-stream.h"
#include "wifi-mac-trailer.h"


/*
 * The state machine for this STA is:
 --------------                                          -----------
 | Associated |   <--------------------      ------->    | Refused |
 --------------                        \    /            -----------
    \                                   \  /
     \    -----------------     -----------------------------
      \-> | Beacon Missed | --> | Wait Association Response |
          -----------------     -----------------------------
                \                       ^
                 \                      |
                  \    -----------------------
                   \-> | Wait Probe Response |
                       -----------------------
 */

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("StaWifiMac");

NS_OBJECT_ENSURE_REGISTERED (StaWifiMac);

TypeId
StaWifiMac::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::StaWifiMac")
    .SetParent<RegularWifiMac> ()
    .SetGroupName ("Wifi")
    .AddConstructor<StaWifiMac> ()
    .AddAttribute ("ProbeRequestTimeout", "The interval between two consecutive probe request attempts.",
                   TimeValue (Seconds (0.05)),
                   MakeTimeAccessor (&StaWifiMac::m_probeRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("AssocRequestTimeout", "The interval between two consecutive assoc request attempts.",
                   TimeValue (Seconds (0.5)),
                   MakeTimeAccessor (&StaWifiMac::m_assocRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("RawDuration", "The duration of one RAW group.",
                   TimeValue (MicroSeconds (102400)),
                   MakeTimeAccessor (&StaWifiMac::GetRawDuration,
                                     &StaWifiMac::SetRawDuration),
                   MakeTimeChecker ())
    .AddAttribute ("MaxMissedBeacons",
                   "Number of beacons which much be consecutively missed before "
                   "we attempt to restart association.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&StaWifiMac::m_maxMissedBeacons),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("ActiveProbing",
                   "If true, we send probe requests. If false, we don't."
                   "NOTE: if more than one STA in your simulation is using active probing, "
                   "you should enable it at a different simulation time for each STA, "
                   "otherwise all the STAs will start sending probes at the same time resulting in collisions. "
                   "See bug 1060 for more info.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&StaWifiMac::SetActiveProbing, &StaWifiMac::GetActiveProbing),
                   MakeBooleanChecker ())
    .AddTraceSource ("Assoc", "Associated with an access point.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_assocLogger),
                     "ns3::Mac48Address::TracedCallback")
    .AddTraceSource ("DeAssoc", "Association with an access point lost.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_deAssocLogger),
                     "ns3::Mac48Address::TracedCallback")
  ;
  return tid;
}

StaWifiMac::StaWifiMac ()
  : m_state (BEACON_MISSED),
    m_probeRequestEvent (),
    m_assocRequestEvent (),
    m_beaconWatchdogEnd (Seconds (0.0))
{
  NS_LOG_FUNCTION (this);
  m_rawStart = false;
  m_dataBuffered = false;
  m_aid = 8192;
  uint32_t cwmin = 15;
  uint32_t cwmax = 1023;
  m_pspollDca = CreateObject<DcaTxop> ();
  m_pspollDca->SetAifsn (2);
  m_pspollDca->SetMinCw ((cwmin + 1) / 4 - 1);
  m_pspollDca->SetMaxCw ((cwmin + 1) / 2 - 1);  //same as AC_VO
  m_pspollDca->SetLow (m_low);
  m_pspollDca->SetManager (m_dcfManager);
  m_pspollDca->SetTxMiddle (m_txMiddle);
  fasTAssocType = false; //centraied control
  fastAssocThreshold = 0; // allow some station to associate at the begining
    Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable> ();
  assocVaule = m_rv->GetValue (0, 999);

  //Let the lower layers know that we are acting as a non-AP STA in
  //an infrastructure BSS.
  SetTypeOfStation (STA);
  m_sendCount = 0;
  m_receiveCount = 0;
}

StaWifiMac::~StaWifiMac ()
{
  NS_LOG_FUNCTION (this);
}

void
StaWifiMac::DoDispose ()
{
  NS_LOG_FUNCTION (this);
  m_pspollDca = 0;
  RegularWifiMac::DoDispose ();
}

uint32_t
StaWifiMac::GetAID (void) const
{
  NS_ASSERT ((1 <= m_aid) && (m_aid<= 8191) || (m_aid == 8192));
  return m_aid;
}
    
Time
StaWifiMac::GetRawDuration (void) const
{
  NS_LOG_FUNCTION (this);
  return m_rawDuration;
}
 
bool
StaWifiMac::Is(uint8_t blockbitmap, uint8_t j)
{
  return (((blockbitmap >> j) & 0x01) == 0x01);
}
    
void
StaWifiMac::SetAID (uint32_t aid)
{
  NS_ASSERT ((1 <= aid) && (aid <= 8191));
  m_aid = aid;

  m_dcfManager->SetID(m_aid-1);
}
    
void
StaWifiMac::SetRawDuration (Time interval)
{
  NS_LOG_FUNCTION (this << interval);
  m_rawDuration = interval;
}
    
void
StaWifiMac::SetDataBuffered()
{
  m_dataBuffered = true;
}
    
void
StaWifiMac::ClearDataBuffered()
{
  m_dataBuffered = false;
}
    
void
StaWifiMac::SetInRAWgroup()
{
  m_inRawGroup = true;
}
    
void
StaWifiMac::UnsetInRAWgroup()
{
  m_inRawGroup = false;
}
        
void
StaWifiMac::SetMaxMissedBeacons (uint32_t missed)
{
  NS_LOG_FUNCTION (this << missed);
  m_maxMissedBeacons = missed;
}

void
StaWifiMac::SetProbeRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_probeRequestTimeout = timeout;
}

void
StaWifiMac::SetAssocRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_assocRequestTimeout = timeout;
}

void
StaWifiMac::StartActiveAssociation (void)
{
  NS_LOG_FUNCTION (this);
  TryToEnsureAssociated ();
}

void
StaWifiMac::SetActiveProbing (bool enable)
{
  NS_LOG_FUNCTION (this << enable);
  if (enable)
    {
      Simulator::ScheduleNow (&StaWifiMac::TryToEnsureAssociated, this);
    }
  else
    {
      m_probeRequestEvent.Cancel ();
    }
  m_activeProbing = enable;
}

bool StaWifiMac::GetActiveProbing (void) const
{
  return m_activeProbing;
}

void
StaWifiMac::SendPspoll (void)
{
//	printf("[JI] SendPsPoll is called\n");
  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_PSPOLL);
  hdr.SetId (GetAID());
  hdr.SetAddr1 (GetBssid());
  hdr.SetAddr2 (GetAddress ());
    
  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (hdr);

  //The standard is not clear on the correct queue for management
  //frames if we are a QoS AP. The approach taken here is to always
  //use the DCF for these regardless of whether we have a QoS
  //association or not.
  m_pspollDca->Queue (packet, hdr);
}
    
void
StaWifiMac::SendPspollIfnecessary (void)
{
  //assume only send one beacon during RAW
  if ( m_rawStart & m_inRawGroup && m_pagedStaRaw && m_dataBuffered )
    {
     // SendPspoll ();  //pspoll not really send, just put ps-poll frame in m_pspollDca queue
    }
  else if (!m_rawStart && m_dataBuffered && !m_outsideRawEvent.IsRunning ()) //in case the next beacon coming during RAW, could it happen?
   {
     // SendPspoll ();
   }
}

void
StaWifiMac::S1gBeaconReceived (void)
{
    if (m_outsideRawEvent.IsRunning ())
     {
        m_outsideRawEvent.Cancel ();          //avoid error when actual beacon interval become shorter, otherwise, AccessAllowedIfRaw will set again after raw starting
        //Simulator::ScheduleNow(&StaWifiMac::OutsideRawStartBackoff, this);
     }
    
  if (m_aid == 8192) // send assoication request when Staion is not assoicated
    {
//    printf("[JI] S1gBeaconReceived is called1\n");
      m_dca->AccessAllowedIfRaw (true);
    }
  else if (m_rawStart & m_inRawGroup && m_pagedStaRaw && m_dataBuffered ) // if m_pagedStaRaw is true, only m_dataBuffered can access channel
    {
      //printf("[JI_LOG] S1gBeaconReceived is called2 error~~~~~~~~~~~~~~~~~~~~~~~\n");
      m_outsideRawEvent = Simulator::Schedule(m_lastRawDurationus, &StaWifiMac::OutsideRawStartBackoff, this);
        
      m_pspollDca->AccessAllowedIfRaw (true);
      m_dca->AccessAllowedIfRaw (false);
      m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);
      StartRawbackoff();
    }
//  else if (m_rawStart && m_inRawGroup && !m_pagedStaRaw  )
	else if (m_rawStart && m_inRawGroup)
    {
//        printf("[JI] S1gBeaconReceived is called3 aid = %d\n", m_aid);
	  m_beaconTime = g_nowBeaconTime;

	  if(m_lastRawDurationus != MilliSeconds(100))
		  m_outsideRawEvent = Simulator::Schedule(m_lastRawDurationus, &StaWifiMac::OutsideRawStartBackoff, this);
	  
      m_pspollDca->AccessAllowedIfRaw (false);
      m_dca->AccessAllowedIfRaw (false);
      m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);

	  
//	  printf("[JI_LOG] scheduleing1 nodeID = %d start Access time : ", m_aid);
//	  std::cout << Simulator::Now().GetSeconds()<< "m_statSlotStart : " << m_statSlotStart << std::endl;

		//if(m_statSlotStart == 0)  //Slot 1은 outSide로
		if(m_statSlotStart == 0 && m_lastRawDurationus != MilliSeconds(0))  //Slot 1은 outSide로
			  Simulator::Schedule(m_statSlotStart, &StaWifiMac::RawSlotStartBackoff, this);
    }
  else if (m_rawStart && !m_inRawGroup) //|| (m_rawStart && m_inRawGroup && m_pagedStaRaw && !m_dataBuffered)
    {
//        printf("[JI] S1gBeaconReceived is called4 aid = %d\n", m_aid);
	  m_beaconTime = g_nowBeaconTime;
	  if(m_lastRawDurationus != MilliSeconds(100))
		  m_outsideRawEvent = Simulator::Schedule(m_lastRawDurationus, &StaWifiMac::OutsideRawStartBackoff, this);

//	  printf("[JI_LOG] scheduleing2 nodeID = %d start Access time : ", m_aid);
//	  std::cout << Simulator::Now().GetSeconds()<< std::endl; 
        
      m_pspollDca->AccessAllowedIfRaw (false);
      m_dca->AccessAllowedIfRaw (false);
      m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
      m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);
//     StartRawbackoff();
    }
    // else (!m_rawStart),  this case cannot happen, since we assume s1g beacon always indicating one raw
    m_rawStart = false;
}

void
StaWifiMac::RawSlotStartBackoff (void)
{
//	  printf("[JI_LOG] RawSlotStartBackoff Start nodeID = %d Access time : ", m_aid);
//	std::cout << Simulator::Now().GetSeconds()<< std::endl; 

	if(m_lastRawDurationus != MilliSeconds(100) && m_lastRawDurationus != MilliSeconds(0))
		//Simulator::Schedule(m_lastRawDurationus - MilliSeconds(2), &StaWifiMac::InsideBackoff, this);
		Simulator::Schedule(m_lastRawDurationus, &StaWifiMac::InsideBackoff, this);

	// modified original
//	if(m_slotDuration != MilliSeconds(100))
//		Simulator::Schedule(m_slotDuration - MilliSeconds(2), &StaWifiMac::InsideBackoff, this);

    m_pspollDca->AccessAllowedIfRaw (true);
    m_dca->AccessAllowedIfRaw (true);
    m_edca.find (AC_VO)->second->AccessAllowedIfRaw (true);
    m_edca.find (AC_VI)->second->AccessAllowedIfRaw (true);
    m_edca.find (AC_BE)->second->AccessAllowedIfRaw (true);
    m_edca.find (AC_BK)->second->AccessAllowedIfRaw (true);
    StartRawbackoff();
}
    
void
StaWifiMac::InsideBackoff (void)
{
 	printf("[JI_LOG] InsideBackoff Start Node ID = %d Access time : ", m_aid);
	std::cout << Simulator::Now().GetSeconds() << " " << m_lastRawDurationus << " " << m_slotDuration << " " << m_slotNum<< std::endl; 

   g_bIsInsideBackoff[m_aid - 1] = true;

   m_pspollDca->AccessAllowedIfRaw (false);
   m_dca->AccessAllowedIfRaw (false);
   m_edca.find (AC_VO)->second->AccessAllowedIfRaw (false);
   m_edca.find (AC_VI)->second->AccessAllowedIfRaw (false);
   m_edca.find (AC_BE)->second->AccessAllowedIfRaw (false);
   m_edca.find (AC_BK)->second->AccessAllowedIfRaw (false);
}
    
    
void
StaWifiMac::StartRawbackoff (void)
{
//	printf("[JI_LOG] StartRawbackoff Start nodeID = %d Access time : ", m_aid);
//	std::cout << Simulator::Now().GetSeconds()<< std::endl; 

  g_bIsInsideBackoff[m_aid - 1] = false;

  m_pspollDca->RawStart (); //not really start raw useless allowedAccessRaw is true;
  m_dca->RawStart ();
  m_edca.find (AC_VO)->second->RawStart ();
  m_edca.find (AC_VI)->second->RawStart ();
  m_edca.find (AC_BE)->second->RawStart ();
  m_edca.find (AC_BK)->second->RawStart ();

}


void
StaWifiMac::OutsideRawStartBackoff (void)
{
if(m_beaconTime != g_nowBeaconTime)
{
	std::cout << "[JI_LOG] OutsideRawStartBackoff Different beacon Time : " << m_beaconTime.GetMilliSeconds() << "	" << g_nowBeaconTime.GetMilliSeconds() << std::endl;
	return;
}
//else
//{
//	std::cout << "[JI_LOG] Same beacon Time : " << m_beaconTime.GetMilliSeconds() << "	" << g_nowBeaconTime.GetMilliSeconds() << std::endl;
//}


//  printf("[JI_LOG] OutsideRawStartBackoff Start Node ID = %d Access time : ", m_aid);
//  std::cout << Simulator::Now().GetSeconds()<< " " << m_lastRawDurationus <<std::endl; 
 
  Simulator::ScheduleNow(&DcaTxop::OutsideRawStart, StaWifiMac::m_pspollDca);
  Simulator::ScheduleNow(&DcaTxop::OutsideRawStart, StaWifiMac::m_dca);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_VO)->second);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_VI)->second);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_BE)->second);
  Simulator::ScheduleNow(&EdcaTxopN::OutsideRawStart, StaWifiMac::m_edca.find (AC_BK)->second);
}
    
void
StaWifiMac::SetWifiRemoteStationManager (Ptr<WifiRemoteStationManager> stationManager)
{
  NS_LOG_FUNCTION (this << stationManager);
  m_pspollDca->SetWifiRemoteStationManager (stationManager);
  RegularWifiMac::SetWifiRemoteStationManager (stationManager);
}
    
void
StaWifiMac::SendProbeRequest (void)
{
  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetProbeReq ();
  hdr.SetAddr1 (Mac48Address::GetBroadcast ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (Mac48Address::GetBroadcast ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtProbeRequestHeader probe;
  probe.SetSsid (GetSsid ());
  probe.SetSupportedRates (GetSupportedRates ());
  if (m_htSupported)
    {
      probe.SetHtCapabilities (GetHtCapabilities ());
      hdr.SetNoOrder ();
    }

  packet->AddHeader (probe);

  //The standard is not clear on the correct queue for management
  //frames if we are a QoS AP. The approach taken here is to always
  //use the DCF for these regardless of whether we have a QoS
  //association or not.
  m_dca->Queue (packet, hdr);

  if (m_probeRequestEvent.IsRunning ())
    {
      m_probeRequestEvent.Cancel ();
    }
  m_probeRequestEvent = Simulator::Schedule (m_probeRequestTimeout,
                                             &StaWifiMac::ProbeRequestTimeout, this);
}

void
StaWifiMac::SendAssociationRequest (void)
{
  NS_LOG_FUNCTION (this << GetBssid ());
  if (!m_s1gSupported)
    {
        fastAssocThreshold = 1023;
    }

if (assocVaule < fastAssocThreshold)
{
  WifiMacHeader hdr;
  hdr.SetAssocReq ();
  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (GetBssid ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtAssocRequestHeader assoc;
  assoc.SetSsid (GetSsid ());
  assoc.SetSupportedRates (GetSupportedRates ());
  if (m_htSupported)
    {
      assoc.SetHtCapabilities (GetHtCapabilities ());
      hdr.SetNoOrder ();
    }

  packet->AddHeader (assoc);

  //The standard is not clear on the correct queue for management
  //frames if we are a QoS AP. The approach taken here is to always
  //use the DCF for these regardless of whether we have a QoS
  //association or not.
  m_dca->Queue (packet, hdr);
}

  if (m_assocRequestEvent.IsRunning ())
    {
      m_assocRequestEvent.Cancel ();
    }
  m_assocRequestEvent = Simulator::Schedule (m_assocRequestTimeout,
                                           &StaWifiMac::AssocRequestTimeout, this);
}

void
StaWifiMac::TryToEnsureAssociated (void)
{
  NS_LOG_FUNCTION (this);
  switch (m_state)
    {
    case ASSOCIATED:
      return;
      break;
    case WAIT_PROBE_RESP:
      /* we have sent a probe request earlier so we
         do not need to re-send a probe request immediately.
         We just need to wait until probe-request-timeout
         or until we get a probe response
       */
      break;
    case BEACON_MISSED:
      /* we were associated but we missed a bunch of beacons
       * so we should assume we are not associated anymore.
       * We try to initiate a probe request now.
       */
      m_linkDown ();
      if (m_activeProbing)
        {
          SetState (WAIT_PROBE_RESP);
          SendProbeRequest ();
        }
      break;
    case WAIT_ASSOC_RESP:
      /* we have sent an assoc request so we do not need to
         re-send an assoc request right now. We just need to
         wait until either assoc-request-timeout or until
         we get an assoc response.
       */
      break;
    case REFUSED:
      /* we have sent an assoc request and received a negative
         assoc resp. We wait until someone restarts an
         association with a given ssid.
       */
      break;
    }
}

void
StaWifiMac::AssocRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_ASSOC_RESP);
  SendAssociationRequest ();
}

void
StaWifiMac::ProbeRequestTimeout (void)
{
  NS_LOG_FUNCTION (this);
  SetState (WAIT_PROBE_RESP);
  SendProbeRequest ();
}

void
StaWifiMac::MissedBeacons (void)
{
  NS_LOG_FUNCTION (this);
  if (m_beaconWatchdogEnd > Simulator::Now ())
    {
      if (m_beaconWatchdog.IsRunning ())
        {
          m_beaconWatchdog.Cancel ();
        }
      m_beaconWatchdog = Simulator::Schedule (m_beaconWatchdogEnd - Simulator::Now (),
                                              &StaWifiMac::MissedBeacons, this);
      return;
    }
  NS_LOG_DEBUG ("beacon missed");
  SetState (BEACON_MISSED);
  TryToEnsureAssociated ();
}

void
StaWifiMac::RestartBeaconWatchdog (Time delay)
{
  NS_LOG_FUNCTION (this << delay);
  m_beaconWatchdogEnd = std::max (Simulator::Now () + delay, m_beaconWatchdogEnd);
  if (Simulator::GetDelayLeft (m_beaconWatchdog) < delay
      && m_beaconWatchdog.IsExpired ())
    {
      NS_LOG_DEBUG ("really restart watchdog.");
      m_beaconWatchdog = Simulator::Schedule (delay, &StaWifiMac::MissedBeacons, this);
    }
}

bool
StaWifiMac::IsAssociated (void) const
{
  return m_state == ASSOCIATED;
}

bool
StaWifiMac::IsWaitAssocResp (void) const
{
  return m_state == WAIT_ASSOC_RESP;
}

void
StaWifiMac::Enqueue (Ptr<const Packet> packet, Mac48Address to)
{
  NS_LOG_FUNCTION (this << packet << to);
  if (!IsAssociated ())
    {
      NotifyTxDrop (packet);
      TryToEnsureAssociated ();
      return;
    }
  WifiMacHeader hdr;

  //If we are not a QoS AP then we definitely want to use AC_BE to
  //transmit the packet. A TID of zero will map to AC_BE (through \c
  //QosUtilsMapTidToAc()), so we use that as our default here.
  uint8_t tid = 0;

  //For now, an AP that supports QoS does not support non-QoS
  //associations, and vice versa. In future the AP model should
  //support simultaneously associated QoS and non-QoS STAs, at which
  //point there will need to be per-association QoS state maintained
  //by the association state machine, and consulted here.
  if (m_qosSupported)
    {
      hdr.SetType (WIFI_MAC_QOSDATA);
      hdr.SetQosAckPolicy (WifiMacHeader::NORMAL_ACK);
      hdr.SetQosNoEosp ();
      hdr.SetQosNoAmsdu ();
      //Transmission of multiple frames in the same TXOP is not
      //supported for now
      hdr.SetQosTxopLimit (0);

      //Fill in the QoS control field in the MAC header
      tid = QosUtilsGetTidForPacket (packet);
      //Any value greater than 7 is invalid and likely indicates that
      //the packet had no QoS tag, so we revert to zero, which'll
      //mean that AC_BE is used.
      if (tid > 7)
        {
          tid = 0;
        }
      hdr.SetQosTid (tid);
    }
  else
    {
      hdr.SetTypeData ();
    }
  if (m_htSupported)
    {
      hdr.SetNoOrder ();
    }

  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (m_low->GetAddress ());
  hdr.SetAddr3 (to);
  hdr.SetDsNotFrom ();
  hdr.SetDsTo ();

  if (m_qosSupported)
    {
      //Sanity check that the TID is valid
      NS_ASSERT (tid < 8);
      m_edca[QosUtilsMapTidToAc (tid)]->Queue (packet, hdr);
    }
  else
    {
      m_dca->Queue (packet, hdr);
    }

//  std::cout << "[JI] STA Send hdr " << m_aid << " " << m_sendCount << " " << hdr << std::endl;
 // WifiMacTrailer fcs;
//  uint32_t fullPacketSize = hdr.GetSerializedSize () + packet->GetSize () + fcs.GetSerializedSize ();
 // std::cout << "[JI] STA Send packet " << m_aid << " "  << m_sendCount++ << " Total length:" << fullPacketSize << std::endl;
/*
  int size = packet->GetSize();
  printf("packet Size :%d " ,size);
  uint8_t buffer[1000] = {0,};
  packet->CopyData(buffer, size);
  for(int i = 0; i < size; i++)
	  printf("%02x ", buffer[i]);
  printf("\n");
*/
}

void
StaWifiMac::Receive (Ptr<Packet> packet, const WifiMacHeader *hdr)
{
//	std::cout << "[JI] STA Receive hdr " << m_aid << " "  << m_receiveCount << " " << *hdr << std::endl;
//	WifiMacTrailer fcs;
//	uint32_t fullPacketSize = hdr->GetSerializedSize () + packet->GetSize () + fcs.GetSerializedSize ();
//	std::cout << "[JI] STA Receive packet " << m_aid << " "  << m_receiveCount++ << " Total length:" << fullPacketSize << std::endl;
/*
	int size = packet->GetSize();
	printf("packet Size :%d " ,size);
	uint8_t buffer[1000] = {0,};
	packet->CopyData(buffer, size);
	for(int i = 0; i < size; i++)
		printf("%02x ", buffer[i]);
	printf("\n");
*/
  NS_LOG_FUNCTION (this << packet << hdr);
  NS_ASSERT (!hdr->IsCtl ());
  if (hdr->GetAddr3 () == GetAddress ())
    {
      NS_LOG_LOGIC ("packet sent by us.");
      return;
    }
  else if (hdr->GetAddr1 () != GetAddress ()
           && !hdr->GetAddr1 ().IsGroup ())
    {
      NS_LOG_LOGIC ("packet is not for us");
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsData ())
    {
      if (!IsAssociated ())
        {
          NS_LOG_LOGIC ("Received data frame while not associated: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (!(hdr->IsFromDs () && !hdr->IsToDs ()))
        {
          NS_LOG_LOGIC ("Received data frame not from the DS: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->GetAddr2 () != GetBssid ())
        {
          NS_LOG_LOGIC ("Received data frame not from the BSS we are associated with: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->IsQosData ())
        {
          if (hdr->IsQosAmsdu ())
            {
              NS_ASSERT (hdr->GetAddr3 () == GetBssid ());
              DeaggregateAmsduAndForward (packet, hdr);
              packet = 0;
            }
          else
            {
              ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
            }
        }
      else
        {
          ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
        }
      return;
    }
  else if (hdr->IsProbeReq ()
           || hdr->IsAssocReq ())
    {
      //This is a frame aimed at an AP, so we can safely ignore it.
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsBeacon ())
    {
      MgtBeaconHeader beacon;
      packet->RemoveHeader (beacon);
      bool goodBeacon = false;
      if (GetSsid ().IsBroadcast ()
          || beacon.GetSsid ().IsEqual (GetSsid ()))
        {
          goodBeacon = true;
        }
      SupportedRates rates = beacon.GetSupportedRates ();
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          uint32_t selector = m_phy->GetBssMembershipSelector (i);
          if (!rates.IsSupportedRate (selector))
            {
              goodBeacon = false;
            }
        }
      if ((IsWaitAssocResp () || IsAssociated ()) && hdr->GetAddr3 () != GetBssid ())
        {
          goodBeacon = false;
        }
      if (goodBeacon)
        {
          Time delay = MicroSeconds (beacon.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          SetBssid (hdr->GetAddr3 ());
        }
      if (goodBeacon && m_state == BEACON_MISSED)
        {
          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }
      return;
    }
  else if (hdr->IsS1gBeacon ())
    {
      S1gBeaconHeader beacon;
      packet->RemoveHeader (beacon);
      bool goodBeacon = false;
    if ((IsWaitAssocResp () || IsAssociated ()) && hdr->GetAddr3 () != GetBssid ()) // for debug
     {
       goodBeacon = false;
     }
    else
     {
      goodBeacon = true;
     }
    if (goodBeacon)
     {
       Time delay = MicroSeconds (beacon.GetBeaconCompatibility().GetBeaconInterval () * m_maxMissedBeacons);
 //      std::cout << "[JI]StaWifiMac::Receive m_maxMissedBeacons : " << m_maxMissedBeacons << " Delay Time : " << delay << std::endl;
       RestartBeaconWatchdog (delay);
       //SetBssid (beacon.GetSA ());
       SetBssid (hdr->GetAddr3 ()); //for debug
     }
    if (goodBeacon && m_state == BEACON_MISSED)
     {
  //    std::cout << "[JI] Beacon_Missed"<< std::endl;
       SetState (WAIT_ASSOC_RESP);
       SendAssociationRequest ();
     }
    if (goodBeacon)
     {
        UnsetInRAWgroup ();
        uint8_t * rawassign;
        rawassign = beacon.GetRPS().GetRawAssignment();
        uint8_t raw_len = beacon.GetRPS().GetInformationFieldSize();
        uint8_t rawtypeindex = rawassign[0] & 0x07;
        uint8_t pageindex = rawassign[4] & 0x03;
         
         uint16_t m_rawslot;
         m_rawslot = (uint16_t(rawassign[2]) << 8) | (uint16_t(rawassign[1]));
		 uint8_t m_SlotFormat = uint8_t (m_rawslot >> 15) & 0x0001;
         uint8_t m_slotCrossBoundary = uint8_t (m_rawslot >> 14) & 0x0002;
		// JI added
		 uint8_t reserved = rawassign[12];
		 //printf("[JI] temp = 0x%02x\n", reserved);

         uint16_t m_slotDurationCount;
         //uint16_t m_slotNum;

         NS_ASSERT (m_SlotFormat <= 1);
         
         if (m_SlotFormat == 0)
           {
             m_slotDurationCount = (m_rawslot >> 6) & 0x00ff;
             m_slotNum = m_rawslot & 0x003f;
           }
         else if (m_SlotFormat == 1)
           {
             m_slotDurationCount = (m_rawslot >> 3) & 0x07ff;
             m_slotNum = m_rawslot & 0x0007;
           }

		//m_slotDuration = MicroSeconds(136 + m_slotDurationCount * 6172); //1s
		//m_slotDuration = MicroSeconds(92 + m_slotDurationCount * 1234);	//200ms
		//m_slotDuration = MicroSeconds(46 + m_slotDurationCount * 617); //100ms
		//m_slotDuration = MicroSeconds(74 + m_slotDurationCount * 123);	//20ms

/*
		switch(m_slotNum)
		{
		case 1 : m_slotDuration = MicroSeconds(118 + m_slotDurationCount * 61); break;//10ms
		case 2 : m_slotDuration = MicroSeconds(104 + m_slotDurationCount * 308); break;//50ms
		case 5 : m_slotDuration = MicroSeconds(74 + m_slotDurationCount * 123); break;//20ms
		case 10 : m_slotDuration = MicroSeconds(118 + m_slotDurationCount * 61); break;//10ms
		default : break;
		}
*/    
		//m_slotDuration = MicroSeconds(140 + m_slotDurationCount * 30);//5ms
		m_slotDuration = MilliSeconds(reserved);//15ms

		//m_slotDuration = MicroSeconds(118 + m_slotDurationCount * 61);//10ms
		//m_slotDuration = MicroSeconds(74 + m_slotDurationCount * 123);	//20ms
	  	//m_slotDuration = MicroSeconds(30 + m_slotDurationCount * 185); //30ms

        m_lastRawDurationus = m_slotDuration * m_slotNum;

//		printf("[JI] m_slotDurationCount : %d, m_slotNum : %d ", m_slotDurationCount, m_slotNum);
//		std::cout << "m_slotDuration : " << m_slotDuration << "m_lastRawDurationus : " << m_lastRawDurationus << std::endl;
 
         if (pageindex == ((GetAID() >> 11 ) & 0x0003)) //in the page indexed
           {
 //          printf("[JI] pageindex : %d %d \n", pageindex, ((GetAID() >> 11 ) & 0x0003));
             uint8_t rawgroup_l = rawassign[4];
             uint8_t rawgroup_m = rawassign[5];
             uint8_t rawgroup_h = rawassign[6];
             uint32_t rawgroup = (uint32_t(rawassign[6]) << 16) | (uint32_t(rawassign[5]) << 8) | uint32_t(rawassign[4]);
             uint16_t raw_start = (rawgroup >> 2) & 0x000003ff;
             uint16_t raw_end = (rawgroup >> 13) & 0x000003ff;
			 //printf("[JI] raw_start %d raw_end %d getAID : %d getAID03ff : %d\n ", raw_start, raw_end, GetAID(), (GetAID() & 0x03ff));
             if ((raw_start <= (GetAID() & 0x03ff)) && ((GetAID() & 0x03ff) <= raw_end))
               {
                 SetInRAWgroup ();
                   
                 uint16_t statsPerSlot = 0;
                 uint16_t statRawSlot = 0;
                 
                  Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable> ();
                 uint16_t offset = m_rv->GetValue (0, 1023);
                 //offset =0; // for test
                 statsPerSlot = (raw_end - raw_start + 1)/m_slotNum; // slot1개당 몇개의 station이 존재하는지...
                 //statRawSlot = ((GetAID() & 0x03ff)-raw_start)/statsPerSlot;

				 // original
				 //statRawSlot = ((GetAID() & 0x03ff)+offset)%m_slotNum;

				 

				if(1 <= g_noSentCount[GetAID()-1])
				{
				 	statRawSlot = 0;
				}
				else
					statRawSlot = 1;
/*
				 switch(m_slotNum)
				{
				case 1 : statRawSlot = 0; break;//100ms
				case 2 : {
							printf("[JI_LOG] AID = %d reserved %d g_noSentCount %d\n", GetAID(), reserved, g_noSentCount[GetAID()-1]);

							if(reserved <= g_noSentCount[GetAID()-1])
							{
							 	statRawSlot = 0;
							}
							else
								statRawSlot = 1;
						 } 
							break;//50ms
				case 5 : {
							if(GetAID() <= 50)
								statRawSlot = 0; 
							 else if(GetAID() <= 100)
							 	statRawSlot = 1;
							 else if(GetAID() <= 150)
							 	statRawSlot = 2;
							 else if(GetAID() <= 200)
							 	statRawSlot = 3;
							 else
							 	statRawSlot = 4;
						 } 
							break;//20ms
				case 10 :{
							if(GetAID() <= 25)
								statRawSlot = 0; 
							 else if(GetAID() <= 50)
							 	statRawSlot = 1;
							 else if(GetAID() <= 75)
							 	statRawSlot = 2;
							 else if(GetAID() <= 100)
							 	statRawSlot = 3;
							 else if(GetAID() <= 125)
							 	statRawSlot = 4;
							 else if(GetAID() <= 150)
							 	statRawSlot = 5;
							 else if(GetAID() <= 175)
							 	statRawSlot = 6;
							 else if(GetAID() <= 200)
							 	statRawSlot = 7;
							 else if(GetAID() <= 225)
							 	statRawSlot = 8;
							 else
							 	statRawSlot = 9;
						 }
							break;//10ms
				default : break;
				}
*/			 
//				printf("[JI_LOG] AID = %d reserved %d g_noSentCount %d statRawSlot %d\n", GetAID(), reserved, g_noSentCount[GetAID()-1], statRawSlot);

				//printf("[JI_LOG] (GetAID(%d) GetAID&0x03ff(%d) + offset(%d)) mod m_slotNum(%d) = statRawSlot(%d)\n", GetAID(), (GetAID() & 0x03ff), offset, m_slotNum, statRawSlot);

				 g_statRawSlot[GetAID()-1]= statRawSlot;
				 	
                 
                 
                  //m_statSlotStart = MicroSeconds((74 + m_slotDurationCount * 123)*statRawSlot); //20ms
                //	m_statSlotStart = MicroSeconds((46 + m_slotDurationCount * 617)*statRawSlot); // 100ms
                 //m_statSlotStart = MicroSeconds((136 + m_slotDurationCount * 6172)*statRawSlot); // 1s
 				  //m_statSlotStart = MicroSeconds((92 + m_slotDurationCount * 1234)*statRawSlot); //200ms
/*
		  		switch(m_slotNum)
				{
				case 1 : m_statSlotStart = MicroSeconds((46 + m_slotDurationCount * 617)*statRawSlot); break;//100ms
				case 2 : m_statSlotStart = MicroSeconds((104 + m_slotDurationCount * 308)*statRawSlot); break;//50ms
				case 5 : m_statSlotStart = MicroSeconds((74 + m_slotDurationCount * 123)*statRawSlot); break;//20ms
				case 10 : m_statSlotStart = MicroSeconds((118 + m_slotDurationCount * 61)*statRawSlot); break;//10ms
				default : break;
				}
*/
			  	//m_statSlotStart = MicroSeconds((140 + m_slotDurationCount * 30)*statRawSlot);//5ms
			  	m_statSlotStart = MilliSeconds(reserved*statRawSlot);//50ms
			   	//m_statSlotStart = MicroSeconds((118 + m_slotDurationCount * 61)*statRawSlot);//10ms
			  	//m_statSlotStart = MicroSeconds((74 + m_slotDurationCount * 123)*statRawSlot); //20ms
			  	//m_statSlotStart = MicroSeconds((30 + m_slotDurationCount * 185)*statRawSlot); //30ms

						
                //        printf("[JI] pageindex raw_start : %d %d statsPerSlot %d statRawSlot %d m_slotNum %d ", raw_start, raw_end, statsPerSlot, statRawSlot, m_slotNum);
				//  std::cout << "m_staSlotStart : " << m_statSlotStart << std::endl;
               }
            }
         
         m_rawStart = true;
         if (rawtypeindex == 4) // only support Generic Raw (paged STA RAW or not)
           {
//             printf("[JI] rawtypeindex is 4\n");           
             m_pagedStaRaw = true;
           }
         else
           {
//             printf("[JI] rawtypeindex is %d\n", rawtypeindex);           
             m_pagedStaRaw = false;
           }
         
         
            AuthenticationCtrl AuthenCtrl;
            AuthenCtrl = beacon.GetAuthCtrl ();
            fasTAssocType = AuthenCtrl.GetControlType ();
            if (!fasTAssocType)  //only support centralized cnotrol
             {
               fastAssocThreshold = AuthenCtrl.GetThreshold();
             }
     }
    S1gBeaconReceived ();
    return;
   }
  else if (hdr->IsProbeResp ())
    {
      if (m_state == WAIT_PROBE_RESP)
        {
          MgtProbeResponseHeader probeResp;
          packet->RemoveHeader (probeResp);
          if (!probeResp.GetSsid ().IsEqual (GetSsid ()))
            {
              //not a probe resp for our ssid.
              return;
            }
          SupportedRates rates = probeResp.GetSupportedRates ();
          for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
            {
              uint32_t selector = m_phy->GetBssMembershipSelector (i);
              if (!rates.IsSupportedRate (selector))
                {
                  return;
                }
            }
          SetBssid (hdr->GetAddr3 ());
          Time delay = MicroSeconds (probeResp.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          if (m_probeRequestEvent.IsRunning ())
            {
              m_probeRequestEvent.Cancel ();
            }
          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }
      return;
    }
  else if (hdr->IsAssocResp ())
    {
      if (m_state == WAIT_ASSOC_RESP)
        {
          MgtAssocResponseHeader assocResp;
          packet->RemoveHeader (assocResp);
          if (m_assocRequestEvent.IsRunning ())
            {
              m_assocRequestEvent.Cancel ();
            }
          if (assocResp.GetStatusCode ().IsSuccess ())
            {
              SetState (ASSOCIATED);
              NS_LOG_DEBUG ("assoc completed");
              SetAID (assocResp.GetAID ());
              SupportedRates rates = assocResp.GetSupportedRates ();
              if (m_htSupported)
                {
                  HtCapabilities htcapabilities = assocResp.GetHtCapabilities ();
                  m_stationManager->AddStationHtCapabilities (hdr->GetAddr2 (),htcapabilities);
                }

              for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
                {
                  WifiMode mode = m_phy->GetMode (i);
                  if (rates.IsSupportedRate (mode.GetDataRate ()))
                    {
                      m_stationManager->AddSupportedMode (hdr->GetAddr2 (), mode);
                      if (rates.IsBasicRate (mode.GetDataRate ()))
                        {
                          m_stationManager->AddBasicMode (mode);
                        }
                    }
                }
              if (m_htSupported)
                {
                  HtCapabilities htcapabilities = assocResp.GetHtCapabilities ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      uint8_t mcs = m_phy->GetMcs (i);
                      if (htcapabilities.IsSupportedMcs (mcs))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                          //here should add a control to add basic MCS when it is implemented
                        }
                    }
                }
              if (!m_linkUp.IsNull ())
                {
                  m_linkUp ();
                }
            }
          else
            {
              NS_LOG_DEBUG ("assoc refused");
              SetState (REFUSED);
            }
        }
      return;
    }

  //Invoke the receive handler of our parent class to deal with any
  //other frames. Specifically, this will handle Block Ack-related
  //Management Action frames.
  RegularWifiMac::Receive (packet, hdr);
}

SupportedRates
StaWifiMac::GetSupportedRates (void) const
{
  SupportedRates rates;
  if (m_htSupported)
    {
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          rates.SetBasicRate (m_phy->GetBssMembershipSelector (i));
        }
    }
  for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
    {
      WifiMode mode = m_phy->GetMode (i);
      rates.AddSupportedRate (mode.GetDataRate ());
    }
  return rates;
}

HtCapabilities
StaWifiMac::GetHtCapabilities (void) const
{
  HtCapabilities capabilities;
  capabilities.SetHtSupported (1);
  capabilities.SetLdpc (m_phy->GetLdpc ());
  capabilities.SetShortGuardInterval20 (m_phy->GetGuardInterval ());
  capabilities.SetGreenfield (m_phy->GetGreenfield ());
  for (uint8_t i = 0; i < m_phy->GetNMcs (); i++)
    {
      capabilities.SetRxMcsBitmask (m_phy->GetMcs (i));
    }
  return capabilities;
}

void
StaWifiMac::SetState (MacState value)
{
  if (value == ASSOCIATED
      && m_state != ASSOCIATED)
    {
      m_assocLogger (GetBssid ());
    }
  else if (value != ASSOCIATED
           && m_state == ASSOCIATED)
    {
      m_deAssocLogger (GetBssid ());
    }
  m_state = value;
}

} //namespace ns3

