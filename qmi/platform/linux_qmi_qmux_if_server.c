/******************************************************************************
  @file    linux_qmi_qmux_if_server.c
  @brief   The QMI QMUX linux server

  DESCRIPTION
  Linux-based QMUX multi protection-domain server

  INITIALIZATION AND SEQUENCING REQUIREMENTS
  None

  ---------------------------------------------------------------------------
  Copyright (c) 2009-2014 Qualcomm Technologies, Inc. All Rights Reserved.
  Qualcomm Technologies Proprietary and Confidential.
  ---------------------------------------------------------------------------
******************************************************************************/
#define _GNU_SOURCE

#include <unistd.h>
#include <stddef.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include "comdef.h"
#include "qmi_i.h"
#include "qmi_platform_qmux_if.h"
#include "qmi_platform_qmux_io.h"
#include "qmi_qmux.h"
#include <fcntl.h>
#ifdef FEATURE_QMI_ANDROID
#include <properties.h>
#endif
#ifdef FEATURE_QMI_IWLAN
#include <strings.h>
#endif
#ifdef FEATURE_DATA_LOG_QXDM
#include "diag_lsm.h"
#endif


#define LINUX_QMI_QMUX_MAX_CLIENTS       (51)

#define LINUX_QMUX_CLIENT_UNINITIALIZED  (-2)
#define LINUX_QMUX_CLIENT_CONNECTED      (-1)

/* Macros for Modem port open retry */
#define MAX_RETRY_COUNT               120
#define WAIT_TIME_BEFORE_NEXT_RETRY   500000
static pthread_mutex_t    rmnet_port_open_mutex;

#ifdef FEATURE_DATA_LOG_FILE
pthread_mutex_t qmux_file_log_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE *qmuxd_fptr = NULL;
#endif /* FEATURE_DATA_LOG_FILE */

#define LINUX_QMI_QMUX_SMD_TIMEOUT_CONFIG         "/sys/devices/virtual/smdpkt/smdcntl0/open_timeout"
#define LINUX_QMI_QMUX_SMD_POWER_UP_TIMEOUT       "20"
#define LINUX_QMI_QMUX_SMD_POWER_UP_TIMEOUT_SIZE  (sizeof(LINUX_QMI_QMUX_SMD_POWER_UP_TIMEOUT))
#define LINUX_QMI_QMUX_SMD_DEFAULT_TIMEOUT        "0"
#define LINUX_QMI_QMUX_SMD_DEFAULT_TIMEOUT_SIZE   (sizeof(LINUX_QMI_QMUX_SMD_DEFAULT_TIMEOUT))

#define LINUX_QMI_QMUX_HSIC_TIMEOUT_CONFIG         "/sys/devices/virtual/hsicctl/hsicctl0/modem_wait"
#define LINUX_QMI_QMUX_HSIC_POWER_UP_TIMEOUT       "60"
#define LINUX_QMI_QMUX_HSIC_POWER_UP_TIMEOUT_SIZE  (sizeof(LINUX_QMI_QMUX_HSIC_POWER_UP_TIMEOUT))
#define LINUX_QMI_QMUX_HSIC_DEFAULT_TIMEOUT        "0"
#define LINUX_QMI_QMUX_HSIC_DEFAULT_TIMEOUT_SIZE   (sizeof(LINUX_QMI_QMUX_HSIC_DEFAULT_TIMEOUT))

/*================start - sysfs config related declarations========== */
/* one of these files may hold the data control port string */
#define LINUX_QMI_SYSFS_CONFIG_FILE_1 "/sys/module/f_rmnet/parameters/rmnet_ctl_ch"
#define LINUX_QMI_SYSFS_CONFIG_FILE_2 "/sys/module/rmnet/parameters/rmnet_ctl_ch"
#define LINUX_QMI_CFG_PARAM_LEN 15
/* we currently only support 3 connections */
#define LINUX_QMI_MAX_CONN_SUPPORTED QMI_MAX_CONNECTIONS

#ifdef FEATURE_QMI_ANDROID
#define LINUX_QMUX_PROPERTY_VALUE_SIZE     PROPERTY_VALUE_MAX
#define LINUX_QMI_BASEBAND_PROPERTY        "ro.baseband"
#define LINUX_QMI_BASEBAND_VALUE_MSM       "msm"
#define LINUX_QMI_BASEBAND_VALUE_APQ       "apq"
#define LINUX_QMI_BASEBAND_VALUE_SVLTE1    "svlte1"
#define LINUX_QMI_BASEBAND_VALUE_SVLTE2A   "svlte2a"
#define LINUX_QMI_BASEBAND_VALUE_CSFB      "csfb"
#define LINUX_QMI_BASEBAND_VALUE_SGLTE     "sglte"
#define LINUX_QMI_BASEBAND_VALUE_DSDA      "dsda"
#define LINUX_QMI_BASEBAND_VALUE_DSDA2     "dsda2"
#define LINUX_QMI_BASEBAND_VALUE_MDMUSB    "mdm"

#define LINUX_QMI_PERSIST_RADIO_SGLTE_CSFB "persist.radio.sglte_csfb"
#ifdef FEATURE_QMI_IWLAN
  #define LINUX_QMI_PERSIST_IWLAN_ENABLED    "persist.data.iwlan.enable"
  static char iwlan_prop[LINUX_QMUX_PROPERTY_VALUE_SIZE];
#endif /* FEATURE_QMI_IWLAN */

#define LINUX_QMI_LOG_FILE_PATH            "/data/qmuxd_log.txt"
#else
#define LINUX_QMUX_PROPERTY_VALUE_SIZE     (1)
#define LINUX_QMI_LOG_FILE_PATH            "/var/qmuxd_log.txt"
#endif /* FEATURE_QMI_ANDROID */

#define LINUX_QMI_DEVICE_NAME_SIZE  LINUX_QMUX_PROPERTY_VALUE_SIZE
static char linux_qmi_device[LINUX_QMI_DEVICE_NAME_SIZE] = "";

#define MAIN_THREAD_CONN_ID (LINUX_QMI_MAX_CONN_SUPPORTED)
const char *linux_qmi_thread_state[LINUX_QMI_MAX_CONN_SUPPORTED+1];

/* QMI message transport types */
#define LINUX_QMI_TRANSPORT_UNDEF (0x00)
#define LINUX_QMI_TRANSPORT_SMD   (0x01)
#define LINUX_QMI_TRANSPORT_BAM   (0x02)
#define LINUX_QMI_TRANSPORT_SDIO  (0x04)
#define LINUX_QMI_TRANSPORT_USB   (0x08)
#define LINUX_QMI_TRANSPORT_SMUX  (0x10)
#define LINUX_QMI_TRANSPORT_ALL   (LINUX_QMI_TRANSPORT_SMD |\
                                   LINUX_QMI_TRANSPORT_BAM |\
                                   LINUX_QMI_TRANSPORT_SDIO|\
                                   LINUX_QMI_TRANSPORT_SMUX|\
                                   LINUX_QMI_TRANSPORT_USB)

/*
   following structure holds the relationship between smd data control
   port and qmi connection id. It also contains a boolean member to
   identify if the qmi connection id is enabled or not
*/
typedef struct linux_qmi_conn_id_enablement_s
{
  char data_ctl_port[LINUX_QMI_CFG_PARAM_LEN];
  qmi_connection_id_type qmi_conn_id;
  unsigned char transport;
  unsigned char enabled;
  unsigned char open_at_powerup;
} linux_qmi_conn_id_config_s;
/*
   please make sure to maintain accurate mapping of data_ctl_port
   to qmi_conn_id in the following array
*/
static linux_qmi_conn_id_config_s
linux_qmi_conn_id_enablement_array[LINUX_QMI_MAX_CONN_SUPPORTED] =
{
  /* DATAX_CNTL is a descriptive string used by rmnet probably X=8..15
   * do not exist, and need better naming, but for now, I am using
   * them to complete the array */
  {"DATA5_CNTL",      QMI_CONN_ID_RMNET_0,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA6_CNTL",      QMI_CONN_ID_RMNET_1,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA7_CNTL",      QMI_CONN_ID_RMNET_2,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA8_CNTL",      QMI_CONN_ID_RMNET_3,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA9_CNTL",      QMI_CONN_ID_RMNET_4,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA12_CNTL",     QMI_CONN_ID_RMNET_5,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA13_CNTL",     QMI_CONN_ID_RMNET_6,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA14_CNTL",     QMI_CONN_ID_RMNET_7,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"SMD_RMNET_TETH",  QMI_CONN_ID_RMNET_8,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, FALSE},
  {"DATA15_CNTL",     QMI_CONN_ID_RMNET_9,         (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA16_CNTL",     QMI_CONN_ID_RMNET_10,        (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  {"DATA17_CNTL",     QMI_CONN_ID_RMNET_11,        (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), TRUE, TRUE},
  /* Reverse MSM QMI control ports */
  {"DATA23_CNTL",     QMI_CONN_ID_REV_RMNET_0,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA24_CNTL",     QMI_CONN_ID_REV_RMNET_1,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA25_CNTL",     QMI_CONN_ID_REV_RMNET_2,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA26_CNTL",     QMI_CONN_ID_REV_RMNET_3,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA27_CNTL",     QMI_CONN_ID_REV_RMNET_4,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA28_CNTL",     QMI_CONN_ID_REV_RMNET_5,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA29_CNTL",     QMI_CONN_ID_REV_RMNET_6,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA30_CNTL",     QMI_CONN_ID_REV_RMNET_7,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"DATA31_CNTL",     QMI_CONN_ID_REV_RMNET_8,     (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},

  /* SDIO control ports */
  {"SDIO_DATA1_CNTL",  QMI_CONN_ID_RMNET_SDIO_0,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},
  {"SDIO_DATA2_CNTL",  QMI_CONN_ID_RMNET_SDIO_1,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},
  {"SDIO_DATA3_CNTL",  QMI_CONN_ID_RMNET_SDIO_2,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},
  {"SDIO_DATA4_CNTL",  QMI_CONN_ID_RMNET_SDIO_3,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},
  {"SDIO_DATA5_CNTL",  QMI_CONN_ID_RMNET_SDIO_4,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},
  {"SDIO_DATA6_CNTL",  QMI_CONN_ID_RMNET_SDIO_5,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},
  {"SDIO_DATA7_CNTL",  QMI_CONN_ID_RMNET_SDIO_6,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},
  {"SDIO_DATA8_CNTL",  QMI_CONN_ID_RMNET_SDIO_7,    LINUX_QMI_TRANSPORT_SDIO,                         TRUE, TRUE},

  /* Forward MDM QMI control ports */
  {"MDM_DATA1_CNTL",  QMI_CONN_ID_RMNET_USB_0,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  {"MDM_DATA2_CNTL",  QMI_CONN_ID_RMNET_USB_1,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  {"MDM_DATA3_CNTL",  QMI_CONN_ID_RMNET_USB_2,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  {"MDM_DATA4_CNTL",  QMI_CONN_ID_RMNET_USB_3,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  {"MDM_DATA5_CNTL",  QMI_CONN_ID_RMNET_USB_4,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  {"MDM_DATA6_CNTL",  QMI_CONN_ID_RMNET_USB_5,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  {"MDM_DATA7_CNTL",  QMI_CONN_ID_RMNET_USB_6,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  {"MDM_DATA8_CNTL",  QMI_CONN_ID_RMNET_USB_7,     LINUX_QMI_TRANSPORT_USB,                           TRUE, TRUE},
  /* Reverse MDM QMI control ports */
  {"MDM_DATA9_CNTL",  QMI_CONN_ID_REV_RMNET_USB_0, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA10_CNTL", QMI_CONN_ID_REV_RMNET_USB_1, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA11_CNTL", QMI_CONN_ID_REV_RMNET_USB_2, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA12_CNTL", QMI_CONN_ID_REV_RMNET_USB_3, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA13_CNTL", QMI_CONN_ID_REV_RMNET_USB_4, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA14_CNTL", QMI_CONN_ID_REV_RMNET_USB_5, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA15_CNTL", QMI_CONN_ID_REV_RMNET_USB_6, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA16_CNTL", QMI_CONN_ID_REV_RMNET_USB_7, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},
  {"MDM_DATA17_CNTL", QMI_CONN_ID_REV_RMNET_USB_8, LINUX_QMI_TRANSPORT_USB,                           FALSE, TRUE},

  {"MDM_DATA15_CNTL", QMI_CONN_ID_RMNET_12,        (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},
  {"MDM_DATA16_CNTL", QMI_CONN_ID_RMNET_13,        (LINUX_QMI_TRANSPORT_SMD|LINUX_QMI_TRANSPORT_BAM), FALSE, TRUE},

  {"SMUX0",           QMI_CONN_ID_RMNET_SMUX_0,    LINUX_QMI_TRANSPORT_SMUX,                          TRUE, TRUE},

  {"MDM2_DATA1_CNTL", QMI_CONN_ID_RMNET_MDM2_0,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},
  {"MDM2_DATA2_CNTL", QMI_CONN_ID_RMNET_MDM2_1,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},
  {"MDM2_DATA3_CNTL", QMI_CONN_ID_RMNET_MDM2_2,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},
  {"MDM2_DATA4_CNTL", QMI_CONN_ID_RMNET_MDM2_3,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},
  {"MDM2_DATA5_CNTL", QMI_CONN_ID_RMNET_MDM2_4,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},
  {"MDM2_DATA6_CNTL", QMI_CONN_ID_RMNET_MDM2_5,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},
  {"MDM2_DATA7_CNTL", QMI_CONN_ID_RMNET_MDM2_6,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},
  {"MDM2_DATA8_CNTL", QMI_CONN_ID_RMNET_MDM2_7,    LINUX_QMI_TRANSPORT_SDIO,                          TRUE, TRUE},

  {"QMI_PROXY",       QMI_CONN_ID_PROXY,           LINUX_QMI_TRANSPORT_ALL,                           TRUE, TRUE}
};

/* Client connections: currently, radio [, audio, bluetooth] */
typedef enum
{
  LINUX_QMI_CLIENT_CONNECTION_INVALID = -1,
  LINUX_QMI_CLIENT_CONNECTION_RADIO,

#ifdef FEATURE_QMI_ANDROID
  LINUX_QMI_CLIENT_CONNECTION_AUDIO,
  LINUX_QMI_CLIENT_CONNECTION_BLUETOOTH,
  LINUX_QMI_CLIENT_CONNECTION_GPS,
#endif

  LINUX_QMI_MAX_CLIENT_CONNECTIONS
} linux_qmi_client_connection_t;

typedef struct
{
  linux_qmi_client_connection_t  conn_type;
  const char                     *path;
  int                            fd;
  qmi_service_id_type            srv_allowed;
} linux_qmi_listen_sock_t;

typedef struct
{
  int                      max_listen_fd;
  linux_qmi_listen_sock_t  listeners[LINUX_QMI_MAX_CLIENT_CONNECTIONS];
} linux_qmi_listener_info_t;

static linux_qmi_listener_info_t  linux_qmi_listener_info =
{
  LINUX_QMI_INVALID_FD,  /* max_listen_fd */
  {
    /* listeners[].conn_type                  listeners[].path                         listeners[].fd         listeners[].srvc_allowed */
    { LINUX_QMI_CLIENT_CONNECTION_RADIO,      QMI_QMUX_IF_RADIO_CONN_SOCKET_PATH,      LINUX_QMI_INVALID_FD,  QMI_MAX_SERVICES }

#ifdef FEATURE_QMI_ANDROID
    ,{ LINUX_QMI_CLIENT_CONNECTION_AUDIO,     QMI_QMUX_IF_AUDIO_CONN_SOCKET_PATH,      LINUX_QMI_INVALID_FD,  QMI_CSD_SERVICE  }
    ,{ LINUX_QMI_CLIENT_CONNECTION_BLUETOOTH, QMI_QMUX_IF_BLUETOOTH_CONN_SOCKET_PATH,  LINUX_QMI_INVALID_FD,  QMI_MAX_SERVICES }
    ,{ LINUX_QMI_CLIENT_CONNECTION_GPS,       QMI_QMUX_IF_GPS_CONN_SOCKET_PATH,        LINUX_QMI_INVALID_FD,  QMI_MAX_SERVICES }
#endif
  }
};

/* Macros for updating linux_qmi_conn_id_enablement_array for external MDM */
#define LINUX_QMI_MDM_FWD_BEGIN (QMI_CONN_ID_RMNET_MDM_0)
#define LINUX_QMI_MDM_FWD_END   (QMI_CONN_ID_RMNET_MDM_7)
#define LINUX_QMI_MDM_FWD_SET( i, conn, tran, device )                                            \
      if( LINUX_QMI_MDM_FWD_BEGIN+i <= LINUX_QMI_MDM_FWD_END ) {                                  \
        linux_qmi_conn_id_enablement_array[ LINUX_QMI_MDM_FWD_BEGIN+i ].qmi_conn_id = conn;       \
        linux_qmi_conn_id_enablement_array[ LINUX_QMI_MDM_FWD_BEGIN+i ].transport = tran;         \
        strlcpy( linux_qmi_qmux_io_conn_info[ LINUX_QMI_MDM_FWD_BEGIN+i ].port_id_name,           \
                 device,                                                                          \
                 sizeof(linux_qmi_qmux_io_conn_info[ LINUX_QMI_MDM_FWD_BEGIN+i ].port_id_name) ); \
      }

/* Macros for updating linux_qmi_conn_id_enablement_array for second external modem */
#define LINUX_QMI_QSC_BEGIN (QMI_CONN_ID_RMNET_SMUX_0)
#define LINUX_QMI_QSC_END   (QMI_CONN_ID_RMNET_SMUX_0)
#define LINUX_QMI_QSC_SET( i, conn, tran, device )                                            \
      if( LINUX_QMI_QSC_BEGIN+i <= LINUX_QMI_QSC_END ) {                                      \
        linux_qmi_conn_id_enablement_array[ LINUX_QMI_QSC_BEGIN+i ].qmi_conn_id = conn;       \
        linux_qmi_conn_id_enablement_array[ LINUX_QMI_QSC_BEGIN+i ].transport = tran;         \
        strlcpy( linux_qmi_qmux_io_conn_info[ LINUX_QMI_QSC_BEGIN+i ].port_id_name,           \
                 device,                                                                      \
                 sizeof(linux_qmi_qmux_io_conn_info[ LINUX_QMI_QSC_BEGIN+i ].port_id_name) ); \
      }

#define LINUX_QMI_MDM2_BEGIN (QMI_CONN_ID_RMNET_MDM2_0)
#define LINUX_QMI_MDM2_END   (QMI_CONN_ID_RMNET_MDM2_7)
#define LINUX_QMI_MDM2_SET( i, conn, tran, device )                                            \
      if( LINUX_QMI_MDM2_BEGIN+i <= LINUX_QMI_MDM2_END ) {                                      \
        linux_qmi_conn_id_enablement_array[ LINUX_QMI_MDM2_BEGIN+i ].qmi_conn_id = conn;       \
        linux_qmi_conn_id_enablement_array[ LINUX_QMI_MDM2_BEGIN+i ].transport = tran;         \
        strlcpy( linux_qmi_qmux_io_conn_info[ LINUX_QMI_MDM2_BEGIN+i ].port_id_name,           \
                 device,                                                                      \
                 sizeof(linux_qmi_qmux_io_conn_info[ LINUX_QMI_MDM2_BEGIN+i ].port_id_name) ); \
      }

/*================end - sysfs config related declarations=========== */
typedef struct
{
  qmi_qmux_clnt_id_t             clnt_id;
  linux_qmi_client_connection_t  conn_type;
} linux_qmi_client_connection_info_t;

static linux_qmi_client_connection_info_t linux_qmi_qmux_if_client_id_array [LINUX_QMI_QMUX_MAX_CLIENTS];

static unsigned char   linux_qmi_qmux_if_rx_buf[QMI_MAX_MSG_SIZE];

/* File descriptor sets */
static fd_set master_fd_set, read_fd_set;

/* maximum file descriptor number */
static int max_fd = LINUX_QMI_INVALID_FD;

/* Local function declarations */
static void
linux_qmi_qmux_if_server_init_new_client
(
  int                            listener_fd,
  linux_qmi_client_connection_t  conn_type
);

static void
linux_qmi_qmux_if_delete_client
(
  int fd
);

/* QMUX operational mode */
typedef enum
{
  LINUX_QMI_QMUX_IF_MODE_INVALID = -1,
  LINUX_QMI_QMUX_IF_MODE_POWER_UP,
  LINUX_QMI_QMUX_IF_MODE_NORMAL
} linux_qmi_qmux_if_mode_t;

#if defined(FEATURE_DATA_LOG_QXDM)
  boolean qmi_platform_qxdm_init = FALSE;
#endif
/* Pipe for submitting client cleanup requests to the main processing thread
   from RX thread context */
static int pipefds[2] =
{
  LINUX_QMI_INVALID_FD,
  LINUX_QMI_INVALID_FD
};

/*===========================================================================
                          LOCAL FUNCTION DEFINITIONS
===========================================================================*/
/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_configure_port_timeout
===========================================================================*/
/*!
@brief
  Returns a file descriptor corresponding to the listening socket
  pointed by listen_sock_path

@return
  listener sock_fd corresponding to listen_sock_path on SUCCESS
  otherwise LINUX_QMI_INVALID_FD

@note
  - Side Effects
*/
/*=========================================================================*/
static void
linux_qmi_qmux_if_configure_port_timeout
(
  linux_qmi_qmux_if_mode_t  mode
)
{
  const char *smd_timeout  = LINUX_QMI_QMUX_SMD_DEFAULT_TIMEOUT;
  size_t smd_timeout_size  = LINUX_QMI_QMUX_SMD_DEFAULT_TIMEOUT_SIZE;
  const char *hsic_timeout = LINUX_QMI_QMUX_HSIC_DEFAULT_TIMEOUT;
  size_t hsic_timeout_size = LINUX_QMI_QMUX_HSIC_DEFAULT_TIMEOUT_SIZE;
  int fd_smd = LINUX_QMI_INVALID_FD;
  int fd_hsic = LINUX_QMI_INVALID_FD;
  int rc;


  /* Assign the appropriate timeout value for the given mode */
  if (LINUX_QMI_QMUX_IF_MODE_POWER_UP == mode)
  {
    smd_timeout  = LINUX_QMI_QMUX_SMD_POWER_UP_TIMEOUT;
    smd_timeout_size = LINUX_QMI_QMUX_SMD_POWER_UP_TIMEOUT_SIZE;

    hsic_timeout = LINUX_QMI_QMUX_HSIC_POWER_UP_TIMEOUT;
    hsic_timeout_size = LINUX_QMI_QMUX_HSIC_POWER_UP_TIMEOUT_SIZE;
  }

  QMI_DEBUG_MSG_3("linux_qmi_qmux_if_configure_port_timeout: mode=%d, smd_timeout=%s, hsic_timeout=%s",
                  mode,
                  smd_timeout,
                  hsic_timeout);

  fd_smd = open(LINUX_QMI_QMUX_SMD_TIMEOUT_CONFIG, O_WRONLY);

  if (fd_smd < 0)
  {
    QMI_ERR_MSG_3("linux_qmi_qmux_if_configure_port_timeout: failed to open SMD timeout config=%s "
                  "errno [%d:%s]",
                  LINUX_QMI_QMUX_SMD_TIMEOUT_CONFIG,
                  errno,
                  strerror(errno));
  }
  else
  {
    rc = write(fd_smd, smd_timeout, smd_timeout_size);

    if (rc != (int)smd_timeout_size)
    {
      QMI_ERR_MSG_4("linux_qmi_qmux_if_configure_port_timeout: failed to write SMD config=%s "
                    "rc=%d, errno [%d:%s]",
                    LINUX_QMI_QMUX_SMD_TIMEOUT_CONFIG,
                    rc,
                    errno,
                    strerror(errno));
    }
  }

  fd_hsic = open(LINUX_QMI_QMUX_HSIC_TIMEOUT_CONFIG, O_WRONLY);

  if (fd_hsic < 0)
  {
    QMI_ERR_MSG_3("linux_qmi_qmux_if_configure_port_timeout: failed to open HSIC timeout config=%s "
                  "errno [%d:%s]",
                  LINUX_QMI_QMUX_HSIC_TIMEOUT_CONFIG,
                  errno,
                  strerror(errno));
  }
  else
  {
    rc = write(fd_hsic, hsic_timeout, hsic_timeout_size);

    if (rc != (int)hsic_timeout_size)
    {
      QMI_ERR_MSG_4("linux_qmi_qmux_if_configure_port_timeout: failed to write HSIC config=%s "
                    "rc=%d, errno [%d:%s]",
                    LINUX_QMI_QMUX_HSIC_TIMEOUT_CONFIG,
                    rc,
                    errno,
                    strerror(errno));
    }
  }

  /* Close the fds */
  if (LINUX_QMI_INVALID_FD != fd_smd)
  {
    close(fd_smd);
  }

  if (LINUX_QMI_INVALID_FD != fd_hsic)
  {
    close(fd_hsic);
  }
}

/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_get_listener_socket
===========================================================================*/
/*!
@brief
  Returns a file descriptor corresponding to the listening socket
  pointed by listen_sock_path

@return
  listener sock_fd corresponding to listen_sock_path on SUCCESS
  otherwise LINUX_QMI_INVALID_FD

@note
  - Side Effects
*/
/*=========================================================================*/
static int
linux_qmi_qmux_if_get_listener_socket
(
  const char *listen_sock_path
)
{
  struct sockaddr_un  addr;
  int                 listen_fd = LINUX_QMI_INVALID_FD;
  int                 fd = LINUX_QMI_INVALID_FD;
  int                 len, rc, path_len;

  if (NULL == listen_sock_path)
  {
    QMI_ERR_MSG_0("qmuxd: bad param to linux_qmi_qmux_if_get_listener_socket");
    goto bail;
  }

  /* Get the connection listener socket */
  if ((fd = socket (AF_UNIX, SOCK_STREAM, 0)) < 0)
  {
    QMI_ERR_MSG_3 ("qmuxd: unable to open listener socket, rc=%d, errno=[%d:%s]\n",
                   fd,
                   errno,
                   strerror(errno));
    goto bail;
  }

  /* Unlink socket path name just in case.... */
  unlink (listen_sock_path);

  /* setup for bind */
  memset (&addr,0, sizeof (struct sockaddr_un));
  path_len = strlen (listen_sock_path);
  path_len = MIN(path_len, (int)(sizeof(addr.sun_path)-1));
  addr.sun_family = AF_UNIX;
  memcpy (&addr.sun_path[0], listen_sock_path, path_len);
  addr.sun_path[path_len] = '\0';

  len = offsetof (struct sockaddr_un, sun_path) + path_len;

  QMI_DEBUG_MSG_2 ("qmuxd: addr path=%s, len=%d\n",
                   addr.sun_path,
                   len);

  /* Bind socket to address */
  if ((rc = bind (fd, (struct sockaddr *)&addr, len)) < 0)
  {
    QMI_ERR_MSG_3 ("qmuxd: unable to bind to listener socket, rc=%d, errno=[%d:%s]\n",
                   rc,
                   errno,
                   strerror(errno));
    goto bail;
  }

  /* Allow RW permissions only for user and group */
  if (-1 == chmod(listen_sock_path, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP))
  {
    QMI_ERR_MSG_3 ("qmuxd: unable to chmod listen_sock_path=%s, errno=[%d:%s]\n",
                   listen_sock_path,
                   errno,
                   strerror(errno));
  }

  /* Make socket a listener */
  if ((rc = listen (fd, 5)) < 0)
  {
    QMI_ERR_MSG_3("qmuxd: unable to listen with listener socket, rc=%d, errno=[%d:%s]\n",
                  rc,
                  errno,
                  strerror(errno));
    goto bail;
  }

  listen_fd = fd;

bail:
  if (LINUX_QMI_INVALID_FD == listen_fd)
  {
    if (LINUX_QMI_INVALID_FD != fd)
    {
      close(fd);
    }
  }

  return listen_fd;
}

/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_process_cleanup_requests
===========================================================================*/
/*!
@brief
  Processes client cleanup requests on the pipe read fd

@return
  None

@note
  - Side Effects
*/
/*=========================================================================*/
static void
linux_qmi_qmux_if_process_cleanup_requests
(
  fd_set  *read_fd_set
)
{
  if (!read_fd_set)
  {
    QMI_ERR_MSG_0("qmuxd: bad param to linux_qmi_qmux_if_process_cleanup_requests\n");
    return;
  }

  if (LINUX_QMI_INVALID_FD != pipefds[0] &&
      FD_ISSET(pipefds[0], read_fd_set))
  {
    int fd = LINUX_QMI_INVALID_FD;
    ssize_t nread = 0;

    nread = read(pipefds[0], &fd, sizeof(fd));

    while (nread == sizeof(fd))
    {
      QMI_DEBUG_MSG_1("qmuxd: servicing new cleanup request for fd=%d\n", fd);
      if (LINUX_QMI_INVALID_FD != fd)
      {
        linux_qmi_qmux_if_delete_client (fd);
        FD_CLR(fd, read_fd_set);
      }
      nread = read(pipefds[0], &fd, sizeof(fd));
    }
  }
}

/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_process_listener_socket_requests
===========================================================================*/
/*!
@brief
  Processes new client connection requests on the listener fds

@return
  None

@note
  - Side Effects
*/
/*=========================================================================*/
static void
linux_qmi_qmux_if_process_listener_socket_requests
(
  fd_set  *read_fd_set
)
{
  int  i;

  if (!read_fd_set)
  {
    QMI_ERR_MSG_0("qmuxd: bad param to linux_qmi_qmux_if_process_listener_socket_requests\n");
    return;
  }

  /* Loop through the listener_fds and locate the ready fd */
  for (i = 0; i < LINUX_QMI_MAX_CLIENT_CONNECTIONS; ++i)
  {
    /* Process new client connection request if we get one */
    if (LINUX_QMI_INVALID_FD != linux_qmi_listener_info.listeners[i].fd &&
        FD_ISSET(linux_qmi_listener_info.listeners[i].fd, read_fd_set))
    {
      QMI_DEBUG_MSG_2("qmuxd: servicing new client request on listener_fd=[%d:%s]\n",
                      linux_qmi_listener_info.listeners[i].fd,
                      linux_qmi_listener_info.listeners[i].path);

      linux_qmi_qmux_if_server_init_new_client(linux_qmi_listener_info.listeners[i].fd,
                                               linux_qmi_listener_info.listeners[i].conn_type);
      FD_CLR (linux_qmi_listener_info.listeners[i].fd, read_fd_set);
    }
  }
}


/* reads a data control port string - one character at a time */
static int linux_qmi_read_data_ctl_port
(
  int fd,
  char * buf
)
{
  int bytes_read = 0, temp = 0;
  char ch;

  if (buf == NULL)
    return 0;

  do
  {
    temp = read(fd, &ch, 1);

    if (temp)
    {
      /* we only care about alphanumeric chars
         and '_'; rest are ignored */
      if (('0' <= ch && ch <= '9') ||
          ('a' <= ch && ch <= 'z') ||
          ('A' <= ch && ch <= 'Z') ||
          ('_' == ch))
      {
        bytes_read += temp;
        *buf++ = ch;
        *buf = '\0';
        /* read the delimiter if we reached max len */
        if (bytes_read == LINUX_QMI_CFG_PARAM_LEN)
        {
          read(fd, &ch, 1);
          break;
        }
      }
      else
      {
        break;
      }
    }

  } while ( bytes_read != 0 );

  return bytes_read;
}

/* reads from syscfg files to determine which
   data control ports are in-use by other subsystems */

static void linux_qmi_read_sysfs_config(void)
{
  int fd = 0, i = 0, bytes_read = 0;
  /* buffer to hold the value read */
  char buffer[LINUX_QMI_CFG_PARAM_LEN+1];

  fd = open(LINUX_QMI_SYSFS_CONFIG_FILE_1, O_RDONLY);
  if (fd < 0)
  {
    QMI_ERR_MSG_1("qmuxd: linux_qmi_read_sysfs_config: "\
                  "couldn't open file %s",
                  LINUX_QMI_SYSFS_CONFIG_FILE_1);
    /* if previous file doesn't exist, try another file */
    fd = open(LINUX_QMI_SYSFS_CONFIG_FILE_2, O_RDONLY);
    if (fd < 0)
    {
      QMI_ERR_MSG_1 ("qmuxd: linux_qmi_read_sysfs_config: " \
                     "couldn't open %s",
                     LINUX_QMI_SYSFS_CONFIG_FILE_2);
      return;
    }
  }

  /* read value into local buffer */
  do
  {
    /* reset buffer */
    memset(buffer, 0, LINUX_QMI_CFG_PARAM_LEN+1);

    /* read next data control port from the config file into buffer  */
    bytes_read = linux_qmi_read_data_ctl_port(fd,
                                              buffer);

    if (!bytes_read)
    {
      QMI_DEBUG_MSG_0 ("qmuxd: linux_qmi_read_sysfs_config: "                  \
                       "no more data control ports found in cfg file");
      break;
    }
    else
    {
      QMI_DEBUG_MSG_1 ("qmuxd: linux_qmi_read_sysfs_config: "          \
                       "data control port %s found in cfg file",
                       buffer);
    }

    /* go through list of data control ports and disable
       the one we just read in buffer */
    for(i=0; i<LINUX_QMI_MAX_CONN_SUPPORTED; i++)
    {
      /*
         if a match found, disable corresponding qmi_conn_id
         as it might be used by some other module (like usb rmnet)
      */
      if (!(strcmp(
              linux_qmi_conn_id_enablement_array[i].data_ctl_port,
              buffer)))
      {
        linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
        QMI_DEBUG_MSG_1("qmuxd: linux_qmi_read_sysfs_config: qmi conn id %d "  \
                        "will not be used ",
                        linux_qmi_conn_id_enablement_array[i].qmi_conn_id);
      }
    }

  } while (1); /* infinite loop */

  QMI_DEBUG_MSG_0 ("qmuxd: linux_qmi_read_sysfs_config: successfully " \
                   "configured qmi connection ids");

  return;
}


/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_configure_ports
===========================================================================*/
/*!
@brief
  This function will do target-specific port configuraiton.
  - For Android CSFB target, disable non-SDIO ports.
  - For non-Android targets, unconditionally disable SDIO & USB ports.

@return

@note
  - Side Effects
*/
/*=========================================================================*/
static void linux_qmi_qmux_if_configure_ports( const char * device )
{
  int i;

#ifdef FEATURE_QMI_IWLAN
  boolean is_iwlan_enabled = (0 == strncasecmp(iwlan_prop, "true", 4)) ? TRUE : FALSE;
#endif

#ifdef FEATURE_QMI_ANDROID
  if(strlen(device) > 0)
  {
    /* MSM target only has SMD/BAM transport ports */
    if( !strcmp(device, LINUX_QMI_BASEBAND_VALUE_MSM) )
    {
      for(i = 0; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
      {
#ifdef FEATURE_QMI_IWLAN
        /* If iwlan property is set */
        if (TRUE == is_iwlan_enabled)
        {
          /* Enable reverse SMD/BAM control ports */
          if (i >= QMI_CONN_ID_REV_RMNET_0 && i <= QMI_CONN_ID_REV_RMNET_8)
          {
            QMI_DEBUG_MSG_1 ("qmuxd:  Enable reverse port %d\n", i);
            linux_qmi_conn_id_enablement_array[i].enabled = TRUE;
          }
        }
        else
        {
          /* Disable reverse SMD/BAM control ports */
          if (i >= QMI_CONN_ID_REV_RMNET_0 && i <= QMI_CONN_ID_REV_RMNET_8)
          {
            QMI_DEBUG_MSG_1 ("qmuxd:  Disable reverse port %d\n", i);
            qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
          }
        }
#else
        /* Disable reverse SMD/BAM control ports */
        if (i >= QMI_CONN_ID_REV_RMNET_0 && i <= QMI_CONN_ID_REV_RMNET_8)
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable reverse port %d\n", i);
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
#endif /* FEATURE_QMI_IWLAN */

        /* Disable non-SMD/non-BAM ports */
        if( 0 == ((LINUX_QMI_TRANSPORT_SMD | LINUX_QMI_TRANSPORT_BAM) &
                  linux_qmi_conn_id_enablement_array[i].transport) )
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable port %d\n", i);
          linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
      }
    }
    /* CSFB target has only SDIO transport ports */
    else if( !strcmp(device, LINUX_QMI_BASEBAND_VALUE_CSFB) )
    {
      for(i = 0; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
      {
        /* Disable non-SDIO ports */
        if( 0 == (LINUX_QMI_TRANSPORT_SDIO &
                  linux_qmi_conn_id_enablement_array[i].transport) )
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable port %d\n", i);
          linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
      }
    }
    /* MDMUSB target replaces SDIO transport with USB, suppresses SMD/BAM */
    else if( !strcmp(device, LINUX_QMI_BASEBAND_VALUE_MDMUSB) )
    {
      /* Assign USB transport to required ports */
      LINUX_QMI_MDM_FWD_SET( 0, QMI_CONN_ID_RMNET_USB_0, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "0" );
      LINUX_QMI_MDM_FWD_SET( 1, QMI_CONN_ID_RMNET_USB_1, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "1" );
      LINUX_QMI_MDM_FWD_SET( 2, QMI_CONN_ID_RMNET_USB_2, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "2" );
      LINUX_QMI_MDM_FWD_SET( 3, QMI_CONN_ID_RMNET_USB_3, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "3" );
      LINUX_QMI_MDM_FWD_SET( 4, QMI_CONN_ID_RMNET_USB_4, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "4" );
      LINUX_QMI_MDM_FWD_SET( 5, QMI_CONN_ID_RMNET_USB_5, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "5" );
      LINUX_QMI_MDM_FWD_SET( 6, QMI_CONN_ID_RMNET_USB_6, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "6" );
      LINUX_QMI_MDM_FWD_SET( 7, QMI_CONN_ID_RMNET_USB_7, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "7" );

      /* Disable non-USB transport ports */
      for(i = 0; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
      {
#ifdef FEATURE_QMI_IWLAN
        /* If iwlan property is set */
        if (TRUE == is_iwlan_enabled)
        {
          /* Enable reverse HSIC control ports */
          if (i >= QMI_CONN_ID_REV_RMNET_USB_0 && i <= QMI_CONN_ID_REV_RMNET_USB_8)
          {
            QMI_DEBUG_MSG_1 ("qmuxd:  Enable reverse port %d\n", i);
            linux_qmi_conn_id_enablement_array[i].enabled = TRUE;
          }
        }
        else
        {
          /* Disable reverse HSIC control ports */
          if (i >= QMI_CONN_ID_REV_RMNET_USB_0 && i <= QMI_CONN_ID_REV_RMNET_USB_8)
          {
            QMI_DEBUG_MSG_1 ("qmuxd:  Disable reverse port %d\n", i);
            qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
          }
        }
#else
        /* Disable reverse HSIC control ports */
        if (i >= QMI_CONN_ID_REV_RMNET_USB_0 && i <= QMI_CONN_ID_REV_RMNET_USB_8)
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable reverse port %d\n", i);
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
#endif /* FEATURE_QMI_IWLAN */

        /* Disable non-SDIO ports */
        if( 0 == (LINUX_QMI_TRANSPORT_USB &
                  linux_qmi_conn_id_enablement_array[i].transport) )
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable port %d\n", i);
          linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
      }

      QMI_DEBUG_MSG_0 ("qmuxd: Enabled USB transport on MDM ports\n");
    }
    /* DSDA target replaces SDIO with USB, suppress SMD/BAM, enable SMUX */
    else if(!strcmp(device, LINUX_QMI_BASEBAND_VALUE_DSDA))
    {
      /* Assign USB transport to required ports */
      LINUX_QMI_MDM_FWD_SET( 0, QMI_CONN_ID_RMNET_USB_0, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "0" );
      LINUX_QMI_MDM_FWD_SET( 1, QMI_CONN_ID_RMNET_USB_1, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "1" );
      LINUX_QMI_MDM_FWD_SET( 2, QMI_CONN_ID_RMNET_USB_2, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "2" );
      LINUX_QMI_MDM_FWD_SET( 3, QMI_CONN_ID_RMNET_USB_3, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "3" );

      LINUX_QMI_QSC_SET( 0, QMI_CONN_ID_RMNET_SMUX_0, LINUX_QMI_TRANSPORT_SMUX, SMUX_DEVICE_NAME "32" );

      /* Disable non-USB, non-SMUX transport ports */
      for(i = 0; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
      {
        if( 0 == ((LINUX_QMI_TRANSPORT_USB | LINUX_QMI_TRANSPORT_SMUX) &
                  linux_qmi_conn_id_enablement_array[i].transport) )
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable port %d\n", i);
          linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
      }

      QMI_DEBUG_MSG_0 ("qmuxd: Enabled SMUX/USB transport on QSC/MDM ports\n");
    }
    /* DSDA2 target replaces SDIO with USB, suppress SMD/BAM, enable second USB modem */
    else if(!strcmp(device, LINUX_QMI_BASEBAND_VALUE_DSDA2))
    {
      /* Assign USB transport to required ports */
      LINUX_QMI_MDM_FWD_SET( 0, QMI_CONN_ID_RMNET_USB_0, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "0" );
      LINUX_QMI_MDM_FWD_SET( 1, QMI_CONN_ID_RMNET_USB_1, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "1" );
      LINUX_QMI_MDM_FWD_SET( 2, QMI_CONN_ID_RMNET_USB_2, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "2" );
      LINUX_QMI_MDM_FWD_SET( 3, QMI_CONN_ID_RMNET_USB_3, LINUX_QMI_TRANSPORT_USB, HSIC_DEVICE_NAME "3" );

      /* Assign USB transport to required ports */
      LINUX_QMI_MDM2_SET( 0, QMI_CONN_ID_RMNET_MDM2_0, LINUX_QMI_TRANSPORT_USB, USB_DEVICE_NAME "0" );
      LINUX_QMI_MDM2_SET( 1, QMI_CONN_ID_RMNET_MDM2_1, LINUX_QMI_TRANSPORT_USB, USB_DEVICE_NAME "1" );
      LINUX_QMI_MDM2_SET( 2, QMI_CONN_ID_RMNET_MDM2_2, LINUX_QMI_TRANSPORT_USB, USB_DEVICE_NAME "2" );
      LINUX_QMI_MDM2_SET( 3, QMI_CONN_ID_RMNET_MDM2_3, LINUX_QMI_TRANSPORT_USB, USB_DEVICE_NAME "3" );

      /* Disable all non-USB ports */
      for(i = 0; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
      {
        if( 0 == (LINUX_QMI_TRANSPORT_USB & linux_qmi_conn_id_enablement_array[i].transport) )
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable port %d\n", i);
          linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
      }

      QMI_DEBUG_MSG_0 ("qmuxd: Enabled HSIC/USB transport on MDM/MDM2 ports\n");
    }
    else if(!strcmp(device, LINUX_QMI_BASEBAND_VALUE_SGLTE))
    {
      LINUX_QMI_QSC_SET( 0,
                         QMI_CONN_ID_RMNET_SMUX_0,
                         LINUX_QMI_TRANSPORT_SMUX,
                         SMUX_DEVICE_NAME "32");

      for(i = 0; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
      {
        if( 0 == ((LINUX_QMI_TRANSPORT_SMUX  | LINUX_QMI_TRANSPORT_SMD
                    | LINUX_QMI_TRANSPORT_BAM) &
                  linux_qmi_conn_id_enablement_array[i].transport) )
        {
          QMI_DEBUG_MSG_1 ("qmuxd:  Disable port %d\n", i);
          linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
          qmi_qmux_disable_port (linux_qmi_conn_id_enablement_array[i].qmi_conn_id,TRUE);
        }
      }

      QMI_DEBUG_MSG_0 ("qmuxd: Enabled SMUX transport on MDM port\n");
    }
  }
#else /*FEATURE_QMI_ANDROID*/

  /*  Unconditionally disable SDIO & USB ports for non-Android
   *  targets. This may change in the future
   */
  for(i = 0; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
  {
    if( 0 == ((LINUX_QMI_TRANSPORT_SMD | LINUX_QMI_TRANSPORT_BAM) &
                  linux_qmi_conn_id_enablement_array[i].transport))
    {

      QMI_DEBUG_MSG_1 ("qmuxd:  Disable port %d\n", i);
      linux_qmi_conn_id_enablement_array[i].enabled = FALSE;
      qmi_qmux_disable_port(linux_qmi_conn_id_enablement_array[i].qmi_conn_id, TRUE);
    }
  }
#endif /*FEATURE_QMI_ANDROID*/
}

/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_server_init_pipe
===========================================================================*/
/*!
@brief
  Initialize the pipe for initiating clean-up from RX thread context

@return
  None

@note
  - Side Effects
*/
/*=========================================================================*/
static void
linux_qmi_qmux_if_server_init_pipe(void)
{
  if (pipe2(pipefds, O_NONBLOCK) < 0)
  {
    QMI_ERR_MSG_2 ("qmuxd: pipe2 failed errno [%d:%s]\n",
                   errno,
                   strerror(errno));
    return;
  }

  /* Update the max_fd */
  if (pipefds[1] > max_fd)
  {
    max_fd = pipefds[1];
  }

  /* Set the read fd in the master_fd_set */
  FD_SET (pipefds[0], &master_fd_set);

  QMI_DEBUG_MSG_2 ("qmuxd: Added read pipefd=%d, maxfd=%d\n",
                   pipefds[0],
                   max_fd);
}

/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_submit_cleanup_req
===========================================================================*/
/*!
@brief
  Send a cleanup request for the given fd to the main thread

@return
  None

@note
  - Side Effects
*/
/*=========================================================================*/
static void
linux_qmi_qmux_if_submit_cleanup_req(int fd)
{
  if (LINUX_QMI_INVALID_FD == fd)
  {
    QMI_ERR_MSG_0("linux_qmi_qmux_if_submit_cleanup_req: invalid fd\n");
    return;
  }

  if (LINUX_QMI_INVALID_FD == pipefds[1])
  {
    QMI_ERR_MSG_0("linux_qmi_qmux_if_submit_cleanup_req: cleanup pipe not initialized\n");
    return;
  }

  if (write(pipefds[1], &fd, sizeof(fd)) != sizeof(fd))
  {
    QMI_ERR_MSG_0("linux_qmi_qmux_if_submit_cleanup_req: write failed\n");
  }
  else
  {
    QMI_DEBUG_MSG_1("linux_qmi_qmux_if_submit_cleanup_req: sumitted cleanup req for fd=%d\n",
                    fd);
  }
}

/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_server_init_listeners
===========================================================================*/
/*!
@brief
  Initialize the listener sockets using linux_qmi_listner_info structure

@return
  None

@note
  - Side Effects
*/
/*=========================================================================*/
static void
linux_qmi_qmux_if_server_init_listeners(void)
{
  int  listener_fd, i;

  for (i = 0; i < LINUX_QMI_MAX_CLIENT_CONNECTIONS; ++i)
  {
    listener_fd = linux_qmi_qmux_if_get_listener_socket(linux_qmi_listener_info.listeners[i].path);

    if (LINUX_QMI_INVALID_FD == listener_fd)
    {
      QMI_ERR_MSG_1 ("qmuxd: invalid listener_fd recvd for sock_path=%s\n",
                     linux_qmi_listener_info.listeners[i].path);

      linux_qmi_listener_info.listeners[i].fd = LINUX_QMI_INVALID_FD;
      continue;
    }

    /* Set the max_fd and the bit in the master_fd */
    if (listener_fd > max_fd)
    {
      max_fd = listener_fd;
    }

    FD_SET (listener_fd, &master_fd_set);

    /* Update the max_listener_fd */
    if (listener_fd > linux_qmi_listener_info.max_listen_fd)
    {
      linux_qmi_listener_info.max_listen_fd = listener_fd;
    }

    /* Save the listener fd */
    linux_qmi_listener_info.listeners[i].fd = listener_fd;

    QMI_DEBUG_MSG_4 ("qmuxd: Added listener=[%d:%s], max_listener_fd=%d, maxfd=%d\n",
                     linux_qmi_listener_info.listeners[i].fd,
                     linux_qmi_listener_info.listeners[i].path,
                     linux_qmi_listener_info.max_listen_fd,
                     max_fd);
  }
}

/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_server_alloc_client_id
===========================================================================*/
/*!
@brief
  Allocates a new client ID which is sent to the newly connected client.

@return
  Allocated client ID

*/
/*=========================================================================*/
static qmi_qmux_clnt_id_t
linux_qmi_qmux_if_server_alloc_client_id(void)
{
  static qmi_qmux_clnt_id_t qmux_client_id = 0;
  qmi_qmux_clnt_id_t alloc_client_id;

  alloc_client_id = qmux_client_id++;

  /* Don't return the invalid client ID value, rollover */
  if (qmux_client_id < 0)
  {
    qmux_client_id = 0;
  }

  return alloc_client_id;
}


/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_server_init_new_client
===========================================================================*/
/*!
@brief
  Accepts a new connection request from a client, send a newly allocated
  client ID to the client and updates the local client related information.

@return
  None

@note
  - Side Effects
*/
/*=========================================================================*/
static void
linux_qmi_qmux_if_server_init_new_client
(
  int                            listener_fd,
  linux_qmi_client_connection_t  conn_type
)
{
  int client_fd, rc;
  socklen_t len;
  struct stat stats;
  struct sockaddr_un addr;
  qmi_qmux_clnt_id_t qmux_client_id = 0;

  memset (&addr,0,sizeof(struct sockaddr_un));
  len = sizeof(struct sockaddr_un);

  if (LINUX_QMI_INVALID_FD == listener_fd)
  {
    QMI_ERR_MSG_0 ("qmuxd: unable to accept on invalid listener socket\n");
    return;
  }

  QMI_UPDATE_THREAD_STATE(MAIN_THREAD_CONN_ID, MAIN_THREAD_PROCESS_NEW_CLIENT);

  if ((client_fd = accept (listener_fd, (struct sockaddr *)&addr, &len)) < 0)
  {
    QMI_ERR_MSG_3 ("qmuxd: unable to accept on listener socket, rc=%d, errno=[%d:%s]\n",
                   client_fd,
                   errno,
                   strerror(errno));
    return;
  }

  len -= offsetof (struct sockaddr_un, sun_path);
  addr.sun_path[len] = '\0';


  if (( rc = stat (addr.sun_path, &stats)) < 0)
  {
    QMI_ERR_MSG_2 ("qmuxd: unable to stat client socket file \"%s\", rc = %d\n",
                   addr.sun_path,
                   rc);
    close(client_fd);
    return;
  }

  if (S_ISSOCK (stats.st_mode) == 0)
  {
    QMI_ERR_MSG_1 ("qmuxd: client socket file not a socket file, rc = %d\n",
                   rc);
    close(client_fd);
    return;
  }

  /* No longer need the temp file */
  unlink (addr.sun_path);

  /* Make sure fd is in range */
  if (client_fd >= LINUX_QMI_QMUX_MAX_CLIENTS)
  {
    QMI_ERR_MSG_1 ("qmuxd: client FD out of range = %d\n",
                   client_fd);
    close(client_fd);
    return;
  }

  /* Obtain a client ID for the new client */
  qmux_client_id = linux_qmi_qmux_if_server_alloc_client_id();

  QMI_DEBUG_MSG_2 ("qmuxd: sending qmux_client_id=0x%x to client_fd=%d\n",
                   qmux_client_id,
                   client_fd);

  /* Send the ID to the client */
  if (send (client_fd,
            (void *) &qmux_client_id,
            QMI_QMUX_CLIENT_ID_SIZE,
            0) <= 0)
  {
    QMI_ERR_MSG_2 ("qmuxd: failed to send qmux_client_id=0x%x to client_fd=%d\n",
                   qmux_client_id,
                   client_fd);
    close(client_fd);
    return;
  }

  /* Save the client ID for future validation */
  linux_qmi_qmux_if_client_id_array[client_fd].clnt_id   = qmux_client_id;
  linux_qmi_qmux_if_client_id_array[client_fd].conn_type = conn_type;

  /* Add the new fd to the master fd set */
  FD_SET (client_fd,&master_fd_set);
  if (client_fd > max_fd)
  {
    max_fd = client_fd;
  }

  QMI_DEBUG_MSG_3 ("qmuxd: added new qmux_client_id=0x%x, fd=%d, max_fd=%d\n",
                   qmux_client_id,
                   client_fd,
                   max_fd);
}

/*===========================================================================
                          GLOBAL FUNCTION DEFINITIONS
===========================================================================*/

#if 0
/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_server_validate_client_msg
===========================================================================*/
/*!
@brief
  This function validates if a given QMUX client can send requests to the
  given service_id on the modem

@return
  QMI_NO_ERR on success
  QMI_INTERNAL_ERR otherwise

@note
  - Side Effects
    Sends a message to client process
*/
/*=========================================================================*/
int
linux_qmi_qmux_if_server_validate_client_msg
(
  qmi_qmux_clnt_id_t   qmux_client_id,
  qmi_service_id_type  srvc_id
)
{
  int i;
  linux_qmi_client_connection_t  conn_type = LINUX_QMI_CLIENT_CONNECTION_INVALID;
  int                            rc = QMI_INTERNAL_ERR;

  for (i = 0; i < max_fd; ++i)
  {
    if (linux_qmi_qmux_if_client_id_array[i].clnt_id == qmux_client_id)
    {
      QMI_DEBUG_MSG_3 ("qmuxd: found qmux_client_id=%d at fd=%d, conn_type=%d\n",
                       qmux_client_id,
                       i,
                       linux_qmi_qmux_if_client_id_array[i].conn_type);
      conn_type = linux_qmi_qmux_if_client_id_array[i].conn_type;
      break;
    }
  }

  if (LINUX_QMI_CLIENT_CONNECTION_INVALID == conn_type)
  {
    QMI_ERR_MSG_1 ("qmuxd: failed to find valid connection type for qmux_client_id=%d\n",
                   qmux_client_id);
    goto bail;
  }

  /* Locate the listener information corresponding to conn_type */
  for (i = 0; i < LINUX_QMI_NUM_LISTEN_SOCKS; ++i)
  {
    if (linux_qmi_listener_info.listeners[i].conn_type == conn_type)
    {
      QMI_DEBUG_MSG_3 ("qmuxd: found qmux_client_id=%d at fd=%d, conn_type=%d\n",
                       qmux_client_id,
                       i,
                       linux_qmi_qmux_if_client_id_array[i].conn_type);

      /* Determine if the given srvc_id is valid for conn_type */
      if (linux_qmi_listener_info.listeners[i].srv_allowed == QMI_MAX_SERVICES ||
          linux_qmi_listener_info.listeners[i].srv_allowed == srvc_id)
      {
        rc = QMI_NO_ERR;
      }

      break;
    }
  }

bail:
  return rc;
}
#endif


static void
linux_qmi_qmux_if_delete_client
(
  int fd
)
{
  qmi_qmux_clnt_id_t  clnt_id;

  QMI_DEBUG_MSG_1 ("qmuxd: cleaning up fd=%d\n",fd);

  if (fd < 0 || fd >= LINUX_QMI_QMUX_MAX_CLIENTS)
  {
    QMI_ERR_MSG_2 ("fd is out of range.. fd = %d, MAX is %d\n",fd,LINUX_QMI_QMUX_MAX_CLIENTS);
    return;
  }

  clnt_id = linux_qmi_qmux_if_client_id_array[fd].clnt_id;

  /* Clear the client entries to prevent further transmits to it */
  linux_qmi_qmux_if_client_id_array[fd].clnt_id   = LINUX_QMUX_CLIENT_UNINITIALIZED;
  linux_qmi_qmux_if_client_id_array[fd].conn_type = LINUX_QMI_CLIENT_CONNECTION_INVALID;

  /* Remove qmux client from QMUX */
  if (clnt_id != LINUX_QMUX_CLIENT_UNINITIALIZED)
  {
    QMI_DEBUG_MSG_1 ("qmuxd:  deleting qmux_client_id=0x%x\n", (int) clnt_id);
    qmi_qmux_delete_qmux_client (clnt_id);
  }

  FD_CLR (fd, &master_fd_set);

  /* Find new max_fd */
  if (fd == max_fd)
  {
    int i;
    for (i = linux_qmi_listener_info.max_listen_fd; i < fd; i++)
    {
      if (FD_ISSET(i,&master_fd_set))
      {
        max_fd = i;
      }
    }
  }

  /* Close the fd */
  QMI_DEBUG_MSG_2 ("qmuxd: after closing fd=%d, max_fd is now = %d\n",
                   fd,
                   max_fd);
  close (fd);
}


/*===========================================================================
  FUNCTION  linux_qmi_qmux_if_server_tx_msg
===========================================================================*/
/*!
@brief
  Function to send a message to a particular client process based on the
  passed in qmux_client_id

@return
  QMI_NO_ERR if operation was successful, error code if not.

@note
  - Side Effects
    Sends a message to client process
*/
/*=========================================================================*/
int
linux_qmi_qmux_if_server_tx_msg
(
  qmi_qmux_clnt_id_t  qmux_client_id,
  unsigned char       *msg,
  int                 msg_len
)
{
  int  ret,rc, i;
  linux_qmi_qmux_if_platform_hdr_type p;

  rc = QMI_NO_ERR;

  /* Adjust msg buf pointer and size to add on platform specific header */
  msg -= QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE;
  msg_len += QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE;

  /* Set platform header fields */
  p.total_msg_size = msg_len;
  p.qmux_client_id = qmux_client_id;

  /* Copy header into message buffer */
  memcpy ((void *) msg, (void *)&p, QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE);

  /* Look up fd based on qmux_client_id */
  for (i = linux_qmi_listener_info.max_listen_fd; i <= max_fd; i++)
  {
    if (linux_qmi_qmux_if_client_id_array[i].clnt_id == qmux_client_id)
    {
      QMI_DEBUG_MSG_3 ("qmuxd: TX message on fd=%d, to qmux_client_id=0x%x, len=%d\n",
                       i,
                       (int) qmux_client_id,
                       msg_len);

      if ((ret = send (i,
                      (void *) msg,
                      (size_t) msg_len,
                       MSG_DONTWAIT | MSG_NOSIGNAL)) <= 0)
      {
        QMI_ERR_MSG_4 ("qmuxd: TX failed to qmux_client_id=0x%x, error=%d errno[%d:%s]\n",
                       (int) qmux_client_id,
                       ret,
                       errno,
                       strerror(errno));
        linux_qmi_qmux_if_submit_cleanup_req (i);
        rc = QMI_INTERNAL_ERR;
      }
      else if (ret != msg_len)
      {
        QMI_ERR_MSG_3 ("qmuxd: TX failed (not all bytes sent) to qmux_client_id=0x%x, msg_len=%d, num sent=%d\n",
                              (int)qmux_client_id, msg_len, ret);
        linux_qmi_qmux_if_submit_cleanup_req (i);
        rc = QMI_INTERNAL_ERR;
      }
      break;
    }
  }

  /* If we never found qmux_client_id, report error */
  if ((rc == QMI_NO_ERR) && (i > max_fd))
  {
    QMI_ERR_MSG_1 ("qmuxd: TX error, unable to find fd for qmux_client_id=0x%x\n",qmux_client_id);
    rc = QMI_INTERNAL_ERR;
  }

  return rc;
}



int
linux_qmi_qmux_if_server_process_client_msg
(
  int fd
)
{
  ssize_t buf_size;
  linux_qmi_qmux_if_platform_hdr_type   platform_msg_hdr;
  int rc = TRUE;

  /* Range check fd */
  if (fd >= LINUX_QMI_QMUX_MAX_CLIENTS)
  {
    QMI_ERR_MSG_2 ("fd is out of range.. fd = %d, MAX is %d\n",fd,LINUX_QMI_QMUX_MAX_CLIENTS);
    return rc;
  }

  QMI_UPDATE_THREAD_STATE(MAIN_THREAD_CONN_ID, MAIN_THREAD_CLIENT_RECV);

  if ((buf_size = recv (fd,
                        (void *)&platform_msg_hdr,
                        QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE,
                        0)) <= 0)

  {
    QMI_ERR_MSG_4 ("qmuxd: RX on fd=%d returned error=%d errno[%d:%s]\n",
                   fd,
                   (int)buf_size,
                   errno,
                   strerror(errno));
    if (buf_size == 0) /* Client has gone away */
    {
      rc = FALSE;
    }
  }
  else if (buf_size != QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE)
  {
    QMI_ERR_MSG_2 ("qmuxd: RX on fd=%d didn't return enough header data, size received=%d\n",fd,(int)buf_size);
  }
  else if ((linux_qmi_qmux_if_client_id_array[fd].clnt_id < 0) ||
           (linux_qmi_qmux_if_client_id_array[fd].clnt_id != platform_msg_hdr.qmux_client_id))
  {
    QMI_ERR_MSG_2 ("qmuxd: qmux_client_id=0x%x != platform_msg_hdr.qmux_client_id=0x%x\n",
                   linux_qmi_qmux_if_client_id_array[fd].clnt_id,
                   platform_msg_hdr.qmux_client_id);
    rc = FALSE;
  }
  else
  {
    size_t remaining_bytes;

    QMI_DEBUG_MSG_3 ("qmuxd: RX %d bytes on fd=%d from qmux_client_id=0x%x\n",
                     platform_msg_hdr.total_msg_size,
                     fd,
                     platform_msg_hdr.qmux_client_id);

    /* If the total message size in the header is invalid, discard it */
    if (platform_msg_hdr.total_msg_size <= 0 || platform_msg_hdr.total_msg_size <= (int) QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE)
    {
      QMI_ERR_MSG_3 ("qmuxd: RX on fd=%d, total msg size=%d from qmux client %x invalid, discarding...\n",
                     fd,
                     platform_msg_hdr.total_msg_size,
                     (int)platform_msg_hdr.qmux_client_id);
    }
    /* If message is larger than we can handle, than print error message and read/discard message */
    else if (platform_msg_hdr.total_msg_size > QMI_MAX_MSG_SIZE)
    {
      remaining_bytes = (size_t) platform_msg_hdr.total_msg_size - QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE;

      QMI_ERR_MSG_3 ("qmuxd: RX on fd=%d, msg size=%d from qmux client %x too big, discarding...\n",
                     fd,
                     (int)remaining_bytes,
                     (int)platform_msg_hdr.qmux_client_id);
      /* Discard all of the data in the message */
      while (remaining_bytes > 0 )
      {
        if ((buf_size = recv (fd,
                              (void *)linux_qmi_qmux_if_rx_buf,
                              (size_t) ((remaining_bytes < QMI_MAX_MSG_SIZE) ? remaining_bytes : QMI_MAX_MSG_SIZE),
                              0)) <= 0)
        {
          QMI_ERR_MSG_4 ("qmuxd: RX on fd=%d returned error=%d errno=[%d:%s]\n",
                         fd,
                         (int)buf_size,
                         errno,
                         strerror(errno));
          if (buf_size == 0) /* Client has gone away */
          {
            rc = FALSE;
          }
          break;
        }
        else
        {
          remaining_bytes -= buf_size;
        }
      }
    }
    else /* Message looks good... read in and process as normal */
    {
      remaining_bytes = (size_t) platform_msg_hdr.total_msg_size - QMI_QMUX_IF_PLATFORM_SPECIFIC_HDR_SIZE;

      if ((buf_size = recv (fd,
                            (void *)linux_qmi_qmux_if_rx_buf,
                            remaining_bytes,
                            0)) <= 0)
      {
        QMI_ERR_MSG_2 ("qmuxd: RX returned an error=%d on fd=%d\n",(int)buf_size,fd);
        if (buf_size == 0) /* Client has gone away */
        {
          rc = FALSE;
        }
      }
      else if (buf_size != (ssize_t) remaining_bytes)
      {
        QMI_ERR_MSG_3 ("qmuxd: RX on fd=%d didn't return enough msg data, size rx=%d, expected=%d\n",
                       fd,
                       (int)buf_size,
                       (int)remaining_bytes);
      }
      else
      {
        QMI_UPDATE_THREAD_STATE(MAIN_THREAD_CONN_ID, MAIN_THREAD_MODEM_TX);

        /* Process message */
        qmi_qmux_tx_msg (linux_qmi_qmux_if_client_id_array[fd].clnt_id,
                         linux_qmi_qmux_if_rx_buf,
                         remaining_bytes);
      }
    }
  }
  return rc;
}


/*===========================================================================
  FUNCTION  qmuxd_signal_handler
===========================================================================*/
/*!
@brief
  Callback registered as OS signal handler.

@return
  void

@note

  - Dependencies
    - None

  - Side Effects
    - None
*/
/*=========================================================================*/
static void qmuxd_signal_handler( int sig )
{
  unsigned int i;

  switch (sig)
  {
    case SIGUSR1:
      /* On USR1 signal, print the state of all threads */
      QMI_DEBUG_MSG_0("SIGUSR1 Signal Handler\n");
      for (i = 0; i < sizeof(linux_qmi_thread_state)/sizeof(linux_qmi_thread_state[0]); ++i)
      {
        QMI_DEBUG_MSG_2("QMI RX thread on conn_id=%d state=%s",
                      i,
                      (!linux_qmi_thread_state[i]) ?
                      "UNKNOWN" :
                      linux_qmi_thread_state[i]);
      }
      break;

    default:
      QMI_DEBUG_MSG_1("Unhandled signal=%d\n", sig);
      break;
  }
}


int main(int argc, char *argv[])
{
  int i,j;
  int lowest_qmi_port;
  boolean wait_for_modem = FALSE;
  char args[LINUX_QMUX_PROPERTY_VALUE_SIZE];
  /*Proxy daemon will be disabled by deafult for Non
    Android targets currently. This may change in the
    future.*/
  char sglte_csfb_prop[LINUX_QMUX_PROPERTY_VALUE_SIZE];
  int is_qmiproxy_dmn_expected = FALSE;

  QMI_UPDATE_THREAD_STATE(MAIN_THREAD_CONN_ID, MAIN_THREAD_POWERUP_INIT);

  /* Register signal handler */
  signal(SIGUSR1, qmuxd_signal_handler);

#ifdef FEATURE_DATA_LOG_FILE
  qmuxd_fptr = fopen(LINUX_QMI_LOG_FILE_PATH,"w");
#endif

#ifdef FEATURE_DATA_LOG_QXDM
  /*Initialize Diag services*/
  boolean ret_val = FALSE;
  ret_val = Diag_LSM_Init(NULL);
  qmi_platform_qxdm_init = ret_val;
  if ( !ret_val )
  {
    QMI_ERR_MSG_0 ("qmuxd:  failed on DIAG init!\n");
  }
#endif /* FEATURE_DATA_LOG_QXDM */

#ifdef FEATURE_WAIT_FOR_MODEM_HACK
  sleep( FEATURE_WAIT_FOR_MODEM_HACK );
#endif

#ifdef FEATURE_QMI_ANDROID
  /* Retrieve the Android baseband property */
  memset(&args, 0, sizeof(args));
  property_get(LINUX_QMI_BASEBAND_PROPERTY, args, "");
  if( 0 < strlen(args) )
  {
    QMI_DEBUG_MSG_1 ("qmuxd: Android baseband property[%s]\n", args);
    strlcpy( linux_qmi_device, args, sizeof(linux_qmi_device));
  }

  memset(&sglte_csfb_prop, 0, sizeof(sglte_csfb_prop));
  property_get(LINUX_QMI_PERSIST_RADIO_SGLTE_CSFB, sglte_csfb_prop, "");
  if( 0 < strlen(sglte_csfb_prop) )
  {
    QMI_DEBUG_MSG_1 ("qmuxd: Android sglte_csfb property[%s]\n", sglte_csfb_prop);
  }

  if( (0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_CSFB))    ||
      (0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_SVLTE2A)) ||
      (0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_SGLTE) &&
       0 != strcmp(sglte_csfb_prop, "true")) )
  {
    is_qmiproxy_dmn_expected = TRUE;
  }
  else
  {
    is_qmiproxy_dmn_expected = FALSE;
  }

#ifdef FEATURE_QMI_IWLAN
  memset(iwlan_prop, 0, sizeof(iwlan_prop));
  property_get(LINUX_QMI_PERSIST_IWLAN_ENABLED, iwlan_prop, "");
  if (0 < strlen(iwlan_prop))
  {
    QMI_DEBUG_MSG_1 ("qmuxd: Android iwlan enabled property[%s]\n", iwlan_prop);
  }
#endif /* FEATURE_QMI_IWLAN */
#endif /*FEATURE_QMI_ANDROID*/

  /* Initialize client to file descriptors array */
  for (i = 0; i < LINUX_QMI_QMUX_MAX_CLIENTS; i++)
  {
    linux_qmi_qmux_if_client_id_array[i].clnt_id   = LINUX_QMUX_CLIENT_UNINITIALIZED;
    linux_qmi_qmux_if_client_id_array[i].conn_type = LINUX_QMI_CLIENT_CONNECTION_INVALID;
  }

  pthread_mutex_init(&rmnet_port_open_mutex,NULL);

  (void)qmi_qmux_pwr_up_init();

  QMI_DEBUG_MSG_1("qmuxd:  qmi proxy daemon expected %d", is_qmiproxy_dmn_expected );

  /* read sysfs to omit opening of the qmi connections that
     are used by other clients (like usb rmnet) */
  linux_qmi_read_sysfs_config();

  /* Which ports to enable/disable */
  linux_qmi_qmux_if_configure_ports( linux_qmi_device );

  lowest_qmi_port = 0; // temp solution until final mechanism of specifying valid port ranges for qmuxd is introduced

  for (i = lowest_qmi_port; i < LINUX_QMI_MAX_CONN_SUPPORTED; i++)
  {
    if ( QMI_CONN_ID_PROXY == linux_qmi_conn_id_enablement_array[i].qmi_conn_id && !is_qmiproxy_dmn_expected )
    {
      QMI_DEBUG_MSG_1("qmuxd:  bypassing opening pipe to qmi proxy daemon as qmi proxy not supported %d", i );
      continue;
    }

    /* open the qmi connection only if it is enabled */
    if (linux_qmi_conn_id_enablement_array[i].enabled == TRUE &&
        linux_qmi_conn_id_enablement_array[i].open_at_powerup == TRUE)
    {
      if (linux_qmi_conn_id_enablement_array[i].qmi_conn_id ==
          QMI_CONN_ID_RMNET_0)
      {
        wait_for_modem = TRUE;
      }

#ifdef FEATURE_QMI_ANDROID
      if ((0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_CSFB)) ||
          (0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_SVLTE2A)))
      {
        if (linux_qmi_conn_id_enablement_array[i].qmi_conn_id ==
              QMI_CONN_ID_RMNET_SDIO_0)
        {
          wait_for_modem = TRUE;
        }
      }
      else if ((0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_SGLTE))) {
        if (linux_qmi_conn_id_enablement_array[i].qmi_conn_id ==
              QMI_CONN_ID_RMNET_SMUX_0)
        {
           wait_for_modem = TRUE;
        }
      }
      else if ((0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_DSDA))) {
        if (linux_qmi_conn_id_enablement_array[i].qmi_conn_id == QMI_CONN_ID_RMNET_SMUX_0 ||
            linux_qmi_conn_id_enablement_array[i].qmi_conn_id == QMI_CONN_ID_RMNET_USB_0)
        {
           wait_for_modem = TRUE;
        }
      }
      else if ((0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_DSDA2))) {
        if (linux_qmi_conn_id_enablement_array[i].qmi_conn_id == QMI_CONN_ID_RMNET_USB_0 ||
            linux_qmi_conn_id_enablement_array[i].qmi_conn_id == QMI_CONN_ID_RMNET_MDM2_0)
        {
           wait_for_modem = TRUE;
        }
      }
      else if ((0 == strcmp(args, LINUX_QMI_BASEBAND_VALUE_MDMUSB))) {
        if (linux_qmi_conn_id_enablement_array[i].qmi_conn_id ==
              QMI_CONN_ID_RMNET_USB_0)
        {
           wait_for_modem = TRUE;
        }
      }
#endif /* FEATURE_QMI_ANDROID */

      QMI_DEBUG_MSG_4("qmuxd: processing device[%s] connid[%d] transport[0x%x] wait[%d]",
                      linux_qmi_conn_id_enablement_array[i].data_ctl_port,
                      linux_qmi_conn_id_enablement_array[i].qmi_conn_id,
                      linux_qmi_conn_id_enablement_array[i].transport,
                      wait_for_modem );

      if (wait_for_modem)
      {
        pthread_mutex_lock(&rmnet_port_open_mutex);
        for (j =0; j < MAX_RETRY_COUNT; j++)
        {
          /* Use the power-up port timeout value */
          linux_qmi_qmux_if_configure_port_timeout(LINUX_QMI_QMUX_IF_MODE_POWER_UP);

          if (qmi_qmux_open_connection (
              linux_qmi_conn_id_enablement_array[i].qmi_conn_id) !=
              QMI_NO_ERR)
          {
            QMI_ERR_MSG_3 ("qmuxd:  opening connection for RMNET%d failed on attempt %d, "
                           "Waiting %d milliseconds before retry\n",
                           linux_qmi_conn_id_enablement_array[i].qmi_conn_id, j,
                           WAIT_TIME_BEFORE_NEXT_RETRY/1000);
            usleep(WAIT_TIME_BEFORE_NEXT_RETRY);
            continue;
          }
          else
          {
            QMI_DEBUG_MSG_1("qmuxd:  successfully opened qmi conn id %d",
                            linux_qmi_conn_id_enablement_array[i].qmi_conn_id);
            /* even if we are able to open first port successfully
             * wait a bit before trying to open the next one to give
             * enought time to modem to open them all */
            usleep(WAIT_TIME_BEFORE_NEXT_RETRY);
            break;
          }
        }
        pthread_mutex_unlock(&rmnet_port_open_mutex);
        wait_for_modem = FALSE;
        continue;
      }

      if (qmi_qmux_open_connection (
            linux_qmi_conn_id_enablement_array[i].qmi_conn_id) !=
          QMI_NO_ERR)
      {
        QMI_ERR_MSG_1 ("qmuxd:  Opening connection for RMNET%d failed\n",
                       linux_qmi_conn_id_enablement_array[i].qmi_conn_id);
      }
      else
      {
        QMI_DEBUG_MSG_1("qmuxd:  successfully opened qmi conn id %d",
                        linux_qmi_conn_id_enablement_array[i].qmi_conn_id);
      }
    }
    else
    {
      QMI_DEBUG_MSG_3("qmuxd:  skipping opening conn_id=%d, enabled=%d, open_at_powerup=%d",
                      linux_qmi_conn_id_enablement_array[i].qmi_conn_id,
                      linux_qmi_conn_id_enablement_array[i].enabled,
                      linux_qmi_conn_id_enablement_array[i].open_at_powerup);
    }
  }

  /* Initialize file desciptor sets */
  FD_ZERO (&master_fd_set);
  FD_ZERO (&read_fd_set);
  max_fd = 2;

  /* Initialize the pipe for servicing clean-up requests from RX thread context */
  linux_qmi_qmux_if_server_init_pipe();

  /* Set up listerner sockets */
  linux_qmi_qmux_if_server_init_listeners();

  /* Update the port timeout to default */
  linux_qmi_qmux_if_configure_port_timeout(LINUX_QMI_QMUX_IF_MODE_NORMAL);

  for (;;)
  {
    int num_fds_ready;
    int fd;

    read_fd_set = master_fd_set;

    QMI_UPDATE_THREAD_STATE(MAIN_THREAD_CONN_ID, MAIN_THREAD_WAIT_SELECT);

    num_fds_ready = select (max_fd + 1,&read_fd_set,NULL,NULL,NULL);
    if (num_fds_ready < 0)
    {
      QMI_ERR_MSG_1 ("qmuxd: select returns err %d, continuing\n",num_fds_ready);
      continue;
    }

    /* Handle any clean-up requests */
    linux_qmi_qmux_if_process_cleanup_requests(&read_fd_set);

    /* Loop through all client FD's and process any with messages */
    for (fd = linux_qmi_listener_info.max_listen_fd + 1; fd <= max_fd; fd++)
    {
      if (FD_ISSET (fd, &read_fd_set))
      {
        if (linux_qmi_qmux_if_server_process_client_msg(fd) == FALSE)
        {
          linux_qmi_qmux_if_delete_client (fd);
        }
        FD_CLR (fd,&read_fd_set);
      } /* if */
    }  /* for */

    /* Process any requests on the listener fds */
    linux_qmi_qmux_if_process_listener_socket_requests(&read_fd_set);
  }
}

