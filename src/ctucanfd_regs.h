/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * CTU CAN FD register map, from the public CTU CAN FD IP core datasheet
 * (canbus.pages.fel.cvut.cz) and the PCI wrapper layout that QEMU's
 * ctucan_pci models. Every access is a 32-bit aligned word: QEMU 6.2 does not
 * shift narrower writes into place, so 16-bit registers are written through
 * the word that contains them.
 */
#ifndef CTUCANFD_REGS_H
#define CTUCANFD_REGS_H

/* PCI board: BAR0 = bridge id + Cyclone IV CRA, BAR1 = the cores */
#define CTUCAN_PCI_VENDOR       0x1760u     /* TEDIA */
#define CTUCAN_PCI_DEVICE       0xff00u     /* CTU CAN FD PCIe board, bridge v2.1 */
#define CTUCAN_BAR0_ID          0x0000u     /* [3:0] number of cores */
#define CTUCAN_BAR0_CORES(v)    ((v) & 0xfu)
#define CTUCAN_CORE_STRIDE      0x4000u     /* core n at BAR1 + n * 0x4000 */
#define CTUCAN_CORE_SPAN        0x4000u

/* Core registers (byte offsets of 32-bit words) */
#define CTUCAN_DEVICE_ID        0x00u   /* [15:0] DEVICE_ID, [23:16] minor, [31:24] major */
#define CTUCAN_MODE             0x04u   /* MODE [15:0], SETTINGS [31:16] */
#define CTUCAN_STATUS           0x08u
#define CTUCAN_COMMAND          0x0cu
#define CTUCAN_INT_STAT         0x10u   /* write 1 to clear */
#define CTUCAN_INT_ENA_SET      0x14u
#define CTUCAN_INT_ENA_CLR      0x18u
#define CTUCAN_INT_MASK_SET     0x1cu
#define CTUCAN_INT_MASK_CLR     0x20u
#define CTUCAN_BTR              0x24u   /* nominal bit timing */
#define CTUCAN_BTR_FD           0x28u   /* data phase bit timing */
#define CTUCAN_EWL_ERP_FAULT    0x2cu   /* EWL [7:0], ERP [15:8], FAULT_STATE [18:16] */
#define CTUCAN_REC_TEC          0x30u   /* REC [8:0], TEC [24:16] */
#define CTUCAN_ERR_NORM_FD      0x34u   /* ERR_NORM [15:0], ERR_FD [31:16] */
#define CTUCAN_FILTER_CONTROL   0x5cu
#define CTUCAN_RX_MEM_INFO      0x60u   /* RX_BUFF_SIZE [12:0], RX_MEM_FREE [28:16] (words) */
#define CTUCAN_RX_POINTERS      0x64u
#define CTUCAN_RX_STATUS        0x68u   /* RX_STATUS [15:0], RX_SETTINGS [31:16] */
#define CTUCAN_RX_DATA          0x6cu   /* reading pops one word of the RX FIFO */
#define CTUCAN_TX_STATUS        0x70u   /* 4 bits per TXT buffer */
#define CTUCAN_TX_COMMAND       0x74u
#define CTUCAN_TX_PRIORITY      0x78u   /* 3 bits per TXT buffer, nibble aligned */
#define CTUCAN_ERR_CAPT         0x7cu   /* ERR_POS [4:0], ERR_TYPE [7:5], ALC [31:16] */
#define CTUCAN_RX_FR_CTR        0x84u
#define CTUCAN_TX_FR_CTR        0x88u
#define CTUCAN_YOLO             0x90u
#define CTUCAN_TXTB(n)          (0x100u + (unsigned)(n) * 0x100u)  /* TXT buffer n, n = 0..3 */
#define CTUCAN_TXT_BUFFERS      4u

#define CTUCAN_ID_VALUE         0xcafdu
#define CTUCAN_YOLO_VALUE       0xdeadbeefu

/* MODE and SETTINGS, as one word */
#define MODE_RST        (1u << 0)
#define MODE_BMM        (1u << 1)   /* bus monitoring: listen only, no ACK */
#define MODE_STM        (1u << 2)   /* self test: no ACK required */
#define MODE_AFM        (1u << 3)   /* acceptance filters on */
#define MODE_FDE        (1u << 4)   /* CAN FD frames allowed */
#define MODE_ACF        (1u << 7)
#define MODE_TSTM       (1u << 8)
#define SET_RTRLE       (1u << 16)
#define SET_ILBP        (1u << 21)  /* internal loopback */
#define SET_ENA         (1u << 22)  /* controller on the bus */
#define SET_NISOFD      (1u << 23)  /* non-ISO (Bosch) CAN FD CRC */
#define SET_PEX         (1u << 24)

/* STATUS */
#define STAT_RXNE       (1u << 0)
#define STAT_DOR        (1u << 1)
#define STAT_TXNF       (1u << 2)
#define STAT_EFT        (1u << 3)
#define STAT_RXS        (1u << 4)
#define STAT_TXS        (1u << 5)
#define STAT_EWL        (1u << 6)
#define STAT_IDLE       (1u << 7)

/* COMMAND */
#define CMD_RRB         (1u << 2)   /* flush the RX FIFO */
#define CMD_CDO         (1u << 3)   /* clear data overrun */
#define CMD_ERCRST      (1u << 4)   /* leave bus-off after 128 x 11 recessive bits */
#define CMD_RXFCRST     (1u << 5)
#define CMD_TXFCRST     (1u << 6)

/* INT_STAT / INT_ENA_* / INT_MASK_* */
#define INT_RXI         (1u << 0)   /* frame received */
#define INT_TXI         (1u << 1)   /* frame transmitted */
#define INT_EWLI        (1u << 2)   /* error warning limit crossed */
#define INT_DOI         (1u << 3)   /* RX FIFO overrun */
#define INT_FCSI        (1u << 4)   /* fault confinement state changed */
#define INT_ALI         (1u << 5)   /* arbitration lost */
#define INT_BEI         (1u << 6)   /* bus error */
#define INT_OFI         (1u << 7)   /* overload frame */
#define INT_RXFI        (1u << 8)   /* RX FIFO full */
#define INT_BSI         (1u << 9)   /* bit rate shifted */
#define INT_RBNEI       (1u << 10)  /* RX FIFO not empty */
#define INT_TXBHCI      (1u << 11)  /* a TXT buffer changed state */
#define INT_ALL         0xfffu

/* FAULT_STATE bits inside CTUCAN_EWL_ERP_FAULT */
#define FS_ERA          (1u << 16)  /* error active */
#define FS_ERP          (1u << 17)  /* error passive */
#define FS_BOF          (1u << 18)  /* bus off */
#define FS_MASK         (FS_ERA | FS_ERP | FS_BOF)

/* REC_TEC */
#define CNT_REC(v)      ((v) & 0x1ffu)
#define CNT_TEC(v)      (((v) >> 16) & 0x1ffu)

/* RX_MEM_INFO, in 32-bit words */
#define RXMI_SIZE(v)    ((v) & 0x1fffu)
#define RXMI_FREE(v)    (((v) >> 16) & 0x1fffu)

/* RX_STATUS */
#define RXST_RXE        (1u << 0)
#define RXST_RXF        (1u << 1)
#define RXST_RXFRC(v)   (((v) >> 4) & 0x7ffu)   /* frames in the FIFO */

/* TX_STATUS states */
#define TXT_RDY         0x1u
#define TXT_TRAN        0x2u
#define TXT_ABTP        0x3u
#define TXT_TOK         0x4u
#define TXT_ERR         0x6u
#define TXT_ABT         0x7u
#define TXT_ETY         0x8u
#define TXT_PER         0x9u        /* parity error (newer cores) */
#define TXT_STATE(v, n) (((v) >> ((n) * 4u)) & 0xfu)

/* TX_COMMAND */
#define TXC_TXCE        (1u << 0)   /* -> empty */
#define TXC_TXCR        (1u << 1)   /* -> ready */
#define TXC_TXCA        (1u << 2)   /* -> abort */
#define TXC_TXB(n)      (1u << (8u + (n)))
#define TXC_TXB_MASK(m) ((uint32_t)(m) << 8u)   /* m: bit n = TXT buffer n */

/* ERR_CAPT error types */
#define ERRT_BIT        0u
#define ERRT_CRC        1u
#define ERRT_FORM       2u
#define ERRT_ACK        3u
#define ERRT_STUFF      4u
#define ERR_CAPT_TYPE(v) (((v) >> 5) & 0x7u)

/* Frame layout, shared by TXT buffers and the RX FIFO */
#define FRAME_FFW       0x00u   /* frame format word */
#define FRAME_IDW       0x04u   /* identifier word */
#define FRAME_TSL       0x08u
#define FRAME_TSU       0x0cu
#define FRAME_DATA      0x10u   /* up to 16 data words */

#define FFW_DLC_MASK    0xfu
#define FFW_RTR         (1u << 5)
#define FFW_IDE         (1u << 6)
#define FFW_FDF         (1u << 7)
#define FFW_BRS         (1u << 9)
#define FFW_ESI         (1u << 10)
#define FFW_RWCNT_SHIFT 11u         /* words that follow the frame format word */
#define FFW_RWCNT_MASK  0x1fu

/* Identifier word: base id [28:18], extension [17:0]. This is the same layout
 * as a libcan MID, so MIDs pass through unchanged. */
#define IDW_EXT_MASK    0x1fffffffu
#define IDW_STD_MASK    (0x7ffu << 18)

/* BTR and BTR_FD share field positions; widths differ */
#define BTR_PROP_SHIFT  0u
#define BTR_PH1_SHIFT   7u
#define BTR_PH2_SHIFT   13u
#define BTR_BRP_SHIFT   19u
#define BTR_SJW_SHIFT   27u

#endif
