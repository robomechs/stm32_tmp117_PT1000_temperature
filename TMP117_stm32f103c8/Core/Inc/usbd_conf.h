#ifndef __USBD_CONF_H__
#define __USBD_CONF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>
#include "main.h"

#define USBD_MAX_NUM_INTERFACES       1U
#define USBD_MAX_NUM_CONFIGURATION    1U
#define USBD_MAX_STR_DESC_SIZ         0x100U
#define USBD_SUPPORT_USER_STRING_DESC 0U
#define USBD_SELF_POWERED             1U
#define USBD_DEBUG_LEVEL              0U

#define DEVICE_FS                     0U

void *USBD_static_malloc(uint32_t size);
void USBD_static_free(void *p);

#define USBD_malloc                   (uint32_t *)USBD_static_malloc
#define USBD_free                     USBD_static_free
#define USBD_memset                   memset
#define USBD_memcpy                   memcpy
#define USBD_Delay                    HAL_Delay

#if (USBD_DEBUG_LEVEL > 0U)
#define USBD_UsrLog(...)              do { printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define USBD_UsrLog(...)
#endif

#if (USBD_DEBUG_LEVEL > 1U)
#define USBD_ErrLog(...)              do { printf("ERROR: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define USBD_ErrLog(...)
#endif

#if (USBD_DEBUG_LEVEL > 2U)
#define USBD_DbgLog(...)              do { printf("DEBUG: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define USBD_DbgLog(...)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CONF_H__ */

