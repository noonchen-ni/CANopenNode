#ifndef CO_DRIVER_TARGET_H
#define CO_DRIVER_TARGET_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define CO_MULTIPLE_OD
#define CO_CONFIG_NMT 0
#define CO_CONFIG_HB_CONS 0
#define CO_CONFIG_NODE_GUARDING 0
#define CO_CONFIG_EM (CO_CONFIG_EM_PRODUCER | CO_CONFIG_EM_HISTORY | CO_CONFIG_EM_PROD_INHIBIT)
#define CO_CONFIG_SDO_SRV CO_CONFIG_SDO_SRV_SEGMENTED
#define CO_CONFIG_SDO_CLI 0
#define CO_CONFIG_SYNC CO_CONFIG_SYNC_ENABLE
#define CO_CONFIG_PDO (CO_CONFIG_RPDO_ENABLE | CO_CONFIG_TPDO_ENABLE | CO_CONFIG_RPDO_TIMERS_ENABLE \
    | CO_CONFIG_TPDO_TIMERS_ENABLE | CO_CONFIG_PDO_SYNC_ENABLE | CO_CONFIG_PDO_OD_IO_ACCESS)
#define CO_CONFIG_TIME 0
#define CO_CONFIG_LEDS 0
#define CO_CONFIG_LSS CO_CONFIG_LSS_SLAVE
#define CO_CONFIG_GTW 0
#define CO_CONFIG_GFC 0
#define CO_CONFIG_SRDO 0
#define CO_CONFIG_STORAGE 0
#define CO_CONFIG_TRACE 0
#define CO_CONFIG_GLOBAL_FLAG_OD_DYNAMIC 0

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The XNET integration currently supports little-endian targets only
#endif
#define CO_LITTLE_ENDIAN
#define CO_SWAP_16(x) (x)
#define CO_SWAP_32(x) (x)
#define CO_SWAP_64(x) (x)

typedef uint_fast8_t bool_t;
typedef float float32_t;
typedef double float64_t;

typedef struct {
    uint16_t ident;
    uint8_t DLC;
    uint8_t data[8];
} CO_CANrxMsg_t;
#define CO_CANrxMsg_readIdent(msg) (((const CO_CANrxMsg_t*)(msg))->ident)
#define CO_CANrxMsg_readDLC(msg) (((const CO_CANrxMsg_t*)(msg))->DLC)
#define CO_CANrxMsg_readData(msg) (((const CO_CANrxMsg_t*)(msg))->data)

typedef struct {
    uint16_t ident;
    uint16_t mask;
    void* object;
    void (*CANrx_callback)(void* object, void* message);
} CO_CANrx_t;

typedef struct {
    uint32_t ident;
    uint8_t DLC;
    uint8_t data[8];
    volatile bool_t bufferFull;
    volatile bool_t syncFlag;
} CO_CANtx_t;

typedef struct {
    void* CANptr;
    CO_CANrx_t* rxArray;
    uint16_t rxSize;
    CO_CANtx_t* txArray;
    uint16_t txSize;
    uint16_t CANerrorStatus;
    volatile bool_t CANnormal;
} CO_CANmodule_t;

typedef struct {
    void* addr;
    size_t len;
    uint8_t subIndexOD;
    uint8_t attr;
} CO_storage_entry_t;

#ifdef __cplusplus
extern "C" {
#endif
void xnet_co_lock(void* context);
void xnet_co_unlock(void* context);
#ifdef __cplusplus
}
#endif
#define CO_LOCK_CAN_SEND(module) xnet_co_lock((module)->CANptr)
#define CO_UNLOCK_CAN_SEND(module) xnet_co_unlock((module)->CANptr)
#define CO_LOCK_EMCY(module) xnet_co_lock((module)->CANptr)
#define CO_UNLOCK_EMCY(module) xnet_co_unlock((module)->CANptr)
#define CO_LOCK_OD(module) xnet_co_lock((module)->CANptr)
#define CO_UNLOCK_OD(module) xnet_co_unlock((module)->CANptr)

/* Receive dispatch and protocol processing run in the same locked worker. */
#define CO_FLAG_READ(flag) ((flag) != NULL)
#define CO_FLAG_SET(flag) ((flag) = (void*)1)
#define CO_FLAG_CLEAR(flag) ((flag) = NULL)

#endif
