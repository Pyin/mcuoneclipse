/**
 * \file
 * \brief This is the implementation of the Nordic Semiconductor nRF24L01+ low level driver.
 * \author (c) 2013 Erich Styger, http://mcuoneclipse.com/
 * \note MIT License (http://opensource.org/licenses/mit-license.html), see 'RNet_License.txt'
 *
 * This module deals with the low level functions of the transceiver.
 */

#include "RNetConf.h"
#if PL_HAS_RADIO
#include "Radio.h"
#include "RadioNRF24.h"
#include "RF1.h"
#include "RMSG.h"
#include "RStdIO.h"
#include "RPHY.h"
#include "WAIT1.h"

#define RADIO_CHANNEL_INIT   1  /* communication channel for init station */
#define RADIO_CHANNEL_READ   2  /* communication channel for read station */

/* macros to configure device either for RX or TX operation */
#define RF1_CONFIG_SETTINGS  (RF1_EN_CRC|RF1_CRCO)
#define TX_POWERUP()         RF1_WriteRegister(RF1_CONFIG, RF1_CONFIG_SETTINGS|RF1_PWR_UP|RF1_PRIM_TX) /* enable 1 byte CRC, power up and set as PTX */
#define RX_POWERUP()         RF1_WriteRegister(RF1_CONFIG, RF1_CONFIG_SETTINGS|RF1_PWR_UP|RF1_PRIM_RX) /* enable 1 byte CRC, power up and set as PRX */

static bool RADIO_isSniffing = FALSE;

static const uint8_t TADDR[5] = {0x11, 0x22, 0x33, 0x44, 0x55}; /* device address */

/* Radio state definitions */
typedef enum RADIO_AppStatusKind {
  RADIO_INITIAL_STATE, /* initial state of the state machine */
  RADIO_RECEIVER_ALWAYS_ON, /* receiver is in RX mode */
  RADIO_TRANSMIT_DATA, /* send data */
  RADIO_WAITING_DATA_SENT, /* wait until data is sent */
  RADIO_TIMEOUT,
  RADIO_READY_FOR_TX_RX_DATA
} RADIO_AppStatusKind;

static RADIO_AppStatusKind RADIO_AppStatus = RADIO_INITIAL_STATE;

/* need to have this in case RF device is still added to project */
static volatile bool RADIO_isrFlag; /* flag set by ISR */

static void Err(unsigned char *msg) {
#if PL_HAS_SHELL
  CLS1_SendStr(msg, CLS1_GetStdio()->stdErr);
#endif
}

/* callback called from radio driver */
void RADIO_OnInterrupt(void) {
  RADIO_isrFlag = TRUE;
}

static uint8_t CheckTx(void) {
  RPHY_PacketDesc packet;
  uint8_t buf[RPHY_BUFFER_SIZE];
  uint8_t size, i, *p;
  uint8_t res = ERR_OK;
  uint8_t TxDataBuffer[RPHY_PAYLOAD_SIZE];
  
  if (RMSG_GetTxMsg(buf, sizeof(buf))==ERR_OK) {
    RF1_StopRxTx();  /* CE low */
    TX_POWERUP();
    /* set up packet structure */
    packet.flags = RPHY_PACKET_FLAGS_NONE;
    packet.data = &buf[0];
    packet.dataSize = sizeof(buf);
    /* get data */
    size = buf[0]; /* first byte in msg queue is size of message itself */
    p = &buf[1]; /* pointer to first data byte */
    i = 0; /* index pointing to payload */
    while(i<sizeof(TxDataBuffer) && size>0) {
      TxDataBuffer[i] = *p;
      size--; i++;
      p++;
    }
    if (size!=0) {
      /* not enough room in transmit buffer */
      Err((unsigned char*)"***ERR: Radio Overflow!\r\n");
      res = ERR_OVERFLOW;
    }
    while(i<sizeof(TxDataBuffer)) { /* fill/initialized unused bytes */
      TxDataBuffer[i] = 0; /* fill it with zeros */
      size--; i++;
      p++;
    }
    if (RADIO_isSniffing) {
      RPHY_SniffPacket(&packet, TRUE); /* sniff outgoing packet */
    }
    RF1_TxPayload(TxDataBuffer, sizeof(TxDataBuffer)); /* send data */
    return ERR_OK;
  } else {
    return ERR_NOTAVAIL; /* no data to send? */
  }
  return res;
}

/* called to check if we have something in the RX queue. If so, we queue it */
static uint8_t CheckRx(void) {
  uint8_t res = ERR_OK;
  uint8_t RxDataBuffer[RPHY_BUFFER_SIZE];
  uint8_t status;
  
  status = RF1_GetStatus();
  if (status&RF1_STATUS_RX_DR) { /* data received interrupt */
    RF1_RxPayload(RPHY_BUF_PAYLOAD_START(RxDataBuffer), RPHY_PAYLOAD_SIZE); /* get payload */
    RF1_ResetStatusIRQ(RF1_STATUS_RX_DR|RF1_STATUS_TX_DS|RF1_STATUS_MAX_RT); /* make sure we reset all flags. Need to have the pipe number too */
  }
  if (status&RF1_STATUS_TX_DS) { /* data sent interrupt */
    RF1_ResetStatusIRQ(RF1_STATUS_TX_DS); /* clear bit */
  }
  if (status&RF1_STATUS_MAX_RT) { /* retry timeout interrupt */
    RF1_ResetStatusIRQ(RF1_STATUS_MAX_RT); /* clear bit */
  }
  /* put message into Rx queue */
  res = RMSG_QueueRxMsg(RxDataBuffer, sizeof(RxDataBuffer), RPHY_PAYLOAD_SIZE);
  if (res!=ERR_OK) {
    Err((unsigned char*)"ERR: Rx Queue full?\r\n");
  }
  return res;
}

static void RADIO_HandleStateMachine(void) {
  uint8_t status;
  
  for(;;) { /* will break/return */
    switch (RADIO_AppStatus) {
      case RADIO_INITIAL_STATE:
        RF1_StopRxTx();  /* will send/receive data later */
        RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON; /* turn receive on */
        break; /* process switch again */
  
      case RADIO_RECEIVER_ALWAYS_ON: /* turn receive on */
        RX_POWERUP();
        RF1_StartRxTx(); /* Listening for packets */
        RADIO_AppStatus = RADIO_READY_FOR_TX_RX_DATA;
        break; /* process switch again */
  
      case RADIO_READY_FOR_TX_RX_DATA: /* we are ready to receive/send data data */
        if (RADIO_isrFlag) { /* Rx interrupt? */
          RADIO_isrFlag = FALSE; /* reset interrupt flag */
          (void)CheckRx(); /* get message */
          RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON; /* continue listening */
          break; /* process switch again */
        }
        if (CheckTx()==ERR_OK) { /* there was data and it has been sent */
          RADIO_AppStatus = RADIO_WAITING_DATA_SENT;
          break; /* process switch again */
        }
        return;
  
      case RADIO_WAITING_DATA_SENT:
        if (RADIO_isrFlag) { /* check if we have received an interrupt: this is either timeout or low level ack */
          RADIO_isrFlag = FALSE; /* reset interrupt flag */
          status = RF1_GetStatus();
          if (status&RF1_STATUS_RX_DR) { /* data received interrupt */
            RF1_ResetStatusIRQ(RF1_STATUS_RX_DR); /* clear bit */
          }
          if (status&RF1_STATUS_TX_DS) { /* data sent interrupt */
            RF1_ResetStatusIRQ(RF1_STATUS_TX_DS); /* clear bit */
          }
          if (status&RF1_STATUS_MAX_RT) { /* retry timeout interrupt */
            RF1_ResetStatusIRQ(RF1_STATUS_MAX_RT); /* clear bit */
            RADIO_AppStatus = RADIO_TIMEOUT; /* timeout */
          } else {
            RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON; /* turn receive on */
          }
          break; /* process switch again */
        }
        return;
        
      case RADIO_TIMEOUT:
        Err((unsigned char*)"ERR: Timeout\r\n");
        RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON; /* turn receive on */
        break; /* process switch again */
  
      default: /* should not happen! */
        return;
    } /* switch */
  } /* for */
}

static RPHY_PacketDesc radioRx;
static uint8_t radioRxBuf[RPHY_BUFFER_SIZE];

/*! 
 * \brief Radio power-on initialization.
 * \return Error code, ERR_OK if everything is ok.
 */
uint8_t RADIO_PowerUp(void) {
  WAIT1_WaitOSms(100); /* the transceiver needs 100 ms power up time */
  RF1_Init(); /* set CE and CSN to initialization value */
  
  RF1_WriteRegister(RF1_RF_SETUP, RF1_RF_SETUP_RF_PWR_0|RF1_RF_SETUP_RF_DR_250);
  RF1_WriteRegister(RF1_RX_PW_P0, RPHY_PAYLOAD_SIZE); /* number of payload bytes we want to send and receive */
  RF1_SetChannel(RADIO_CHANNEL_READ);

  /* Set RADDR and TADDR as the transmit address since we also enable auto acknowledgment */
  RF1_WriteRegisterData(RF1_RX_ADDR_P0, (uint8_t*)TADDR, sizeof(TADDR));
  RF1_WriteRegisterData(RF1_TX_ADDR, (uint8_t*)TADDR, sizeof(TADDR));

  /* Enable RX_ADDR_P0 address matching */
  RF1_WriteRegister(RF1_EN_RXADDR, RF1_EN_RXADDR_ERX_P0); /* enable data pipe 0 */
  
  /* clear interrupt flags */
  RF1_ResetStatusIRQ(RF1_STATUS_RX_DR|RF1_STATUS_TX_DS|RF1_STATUS_MAX_RT);
  
  /* rx/tx mode */
  RF1_EnableAutoAck(RF1_EN_AA_ENAA_P0); /* enable auto acknowledge on pipe 0. RX_ADDR_P0 needs to be equal to TX_ADDR! */
  RF1_WriteRegister(RF1_SETUP_RETR, RF1_SETUP_RETR_ARD_750|RF1_SETUP_RETR_ARC_15); /* Important: need 750 us delay between every retry */
  
  RX_POWERUP();  /* Power up in receiving mode */
  RF1_StartRxTx(); /* Listening for packets */

  RADIO_AppStatus = RADIO_INITIAL_STATE;
  /* init Rx descriptor */
  radioRx.data = &radioRxBuf[0];
  radioRx.dataSize = sizeof(radioRxBuf);
  return ERR_OK;
}

uint8_t RADIO_Process(void) {
  uint8_t res;
  
  RADIO_HandleStateMachine(); /* process state machine */
  /* process received packets */
  res = RPHY_GetPayload(&radioRx); /* get message */
  if (res==ERR_OK) { /* packet received */
    if (RADIO_isSniffing) {
      RPHY_SniffPacket(&radioRx, FALSE); /* sniff incoming packet */
    }
    if (RPHY_OnPacketRx(&radioRx)==ERR_OK) { /* process incoming packets */
      if (radioRx.flags&RPHY_PACKET_FLAGS_ACK) { /* it was an ack! */
        //EVNT_SetEvent(EVNT_RADIO_ACK); /* set event */
      }
    }
  }
  return ERR_OK;
}

#if PL_HAS_SHELL
static const unsigned char *RadioStateStr(RADIO_AppStatusKind state) {
  switch(state) {
    case RADIO_INITIAL_STATE:         return (const unsigned char*)"INITIAL";
    case RADIO_RECEIVER_ALWAYS_ON:    return (const unsigned char*)"ALWAYS_ON";
    case RADIO_TRANSMIT_DATA:         return (const unsigned char*)"TRANSMIT_DATA";
    case RADIO_WAITING_DATA_SENT:     return (const unsigned char*)"WAITING_DATA_SENT";
    case RADIO_READY_FOR_TX_RX_DATA:  return (const unsigned char*)"READY_TX_RX"; 
    default:                          return (const unsigned char*)"UNKNOWN";
  }
  return (const unsigned char*)"UNKNOWN";
}

static void RADIO_PrintHelp(const CLS1_StdIOType *io) {
  CLS1_SendHelpStr((unsigned char*)"radio", (unsigned char*)"Group of radio commands\r\n", io->stdOut);
  CLS1_SendHelpStr((unsigned char*)"  help|status", (unsigned char*)"Shows radio help or status\r\n", io->stdOut);
  CLS1_SendHelpStr((unsigned char*)"  sniff on|off", (unsigned char*)"Turns sniffing on or off\r\n", io->stdOut);
}

static void RADIO_PrintStatus(const CLS1_StdIOType *io) {
  CLS1_SendStatusStr((unsigned char*)"Radio", (unsigned char*)"\r\n", io->stdOut);
  CLS1_SendStatusStr((unsigned char*)"  state", RadioStateStr(RADIO_AppStatus), io->stdOut);
  CLS1_SendStr((unsigned char*)"\r\n", io->stdOut);
  CLS1_SendStatusStr((unsigned char*)"  sniff", RADIO_isSniffing?(unsigned char*)"yes\r\n":(unsigned char*)"no\r\n", io->stdOut);
}

uint8_t RADIO_ParseCommand(const unsigned char *cmd, bool *handled, const CLS1_StdIOType *io) {
  uint8_t res = ERR_OK;

  if (UTIL1_strcmp((char*)cmd, (char*)CLS1_CMD_HELP)==0 || UTIL1_strcmp((char*)cmd, (char*)"radio help")==0) {
    RADIO_PrintHelp(io);
    *handled = TRUE;
  } else if (UTIL1_strcmp((char*)cmd, (char*)CLS1_CMD_STATUS)==0 || UTIL1_strcmp((char*)cmd, (char*)"radio status")==0) {
    RADIO_PrintStatus(io);
    *handled = TRUE;
  } else if (UTIL1_strcmp((char*)cmd, (char*)"radio sniff on")==0) {
    RADIO_isSniffing = TRUE;
    *handled = TRUE;
  } else if (UTIL1_strcmp((char*)cmd, (char*)"radio sniff off")==0) {
    RADIO_isSniffing = FALSE;
    *handled = TRUE;
  }
  return res;
}
#endif

void RADIO_Deinit(void) {
  /* nothing to do */
}

void RADIO_Init(void) {
  RADIO_isSniffing = FALSE;
}
#else
void RADIO_OnInterrupt(void) {
  /* dummy to make linker happy */
}
#endif /* PL_HAS_RADIO */
