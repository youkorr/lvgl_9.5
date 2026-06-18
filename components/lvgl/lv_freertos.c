/**
 * @file lv_freertos.c
 *
 * ESPHome / ESP32: lv_thread_init() allocates the LVGL software draw-thread
 * ("swdraw") stack from INTERNAL SRAM, exactly like upstream LVGL 9.5.
 *
 * Why this matters (the bug this file used to introduce):
 * A previous revision allocated the draw-thread stack from PSRAM via
 * xTaskCreateWithCaps(MALLOC_CAP_SPIRAM) to "save internal SRAM".  On ESP32 a
 * FreeRTOS task whose stack lives in PSRAM is unsafe: whenever the external
 * memory cache is disabled (SPI-flash writes, OTA, some Wi-Fi/RF paths) or an
 * ISR needs the stack, a task running on a PSRAM stack faults with a
 * cache-access error (LoadProhibited).  A short render (simple page) usually
 * finishes inside a safe window, but a long/complex render (chart, settings,
 * ThorVG/Lottie) spends far more time on the stack and reliably overlaps a
 * cache-disabled window -> crash on page switch.  The fault PC lands inside
 * the draw task, so the backtrace/ELF never points at user code.
 *
 * Fix: allocate the task stack in internal SRAM (plain xTaskCreate), which is
 * the only safe place for a FreeRTOS task stack on ESP32.  The dedicated
 * LV_DRAW_THREAD_STACK_SIZE (48 KB on the ThorVG path) stays isolated on this
 * thread, so ThorVG / Lottie / SVG keep their large render stack and ESPHome's
 * loopTask is untouched.
 *
 * Note: the earlier "esp_startup_start_app app_startup.c:86 (res == pdTRUE)"
 * boot assert was NOT caused by this stack: that assert fires during ESP-IDF
 * startup, before the application task (and therefore before LVGL init) runs.
 */

/**
 * Copyright 2023 NXP
 *
 * SPDX-License-Identifier: MIT
 */

/*********************
 *      INCLUDES
 *********************/
/* Compiled via lv_freertos_wrapper.cpp from the component directory (not from
 * LVGL's src/osal/), so LVGL headers are included with paths relative to the
 * LVGL source root.  "atomic.h" intentionally resolves to the component-local
 * shim (components/lvgl/atomic.h). */
#include "src/osal/lv_os_private.h"
#if LV_USE_OS == LV_OS_FREERTOS

#include "atomic.h"

#include "src/tick/lv_tick.h"
#include "src/misc/lv_log.h"
#include "src/core/lv_global.h"

/*********************
 *      DEFINES
 *********************/

#define ulMAX_COUNT 10U

#define globals LV_GLOBAL_DEFAULT()

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void prvRunThread(void * pxArg);

static void prvMutexInit(lv_mutex_t * pxMutex);

static void prvCheckMutexInit(lv_mutex_t * pxMutex);

static void prvCondInit(lv_thread_sync_t * pxCond);

static void prvCheckCondInit(lv_thread_sync_t * pxCond);

static void prvCheckCondInitIsr(lv_thread_sync_t * pxCond);

#if !LV_USE_FREERTOS_TASK_NOTIFY
static void prvTestAndDecrement(lv_thread_sync_t * pxCond,
                                uint32_t ulLocalWaitingThreads);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

#ifdef ESP_PLATFORM
    static portMUX_TYPE critSectionMux = portMUX_INITIALIZER_UNLOCKED;
#endif

/**********************
 *      MACROS
 **********************/

#ifdef ESP_PLATFORM
    #define _enter_critical()   taskENTER_CRITICAL(&critSectionMux);
    #define _exit_critical()    taskEXIT_CRITICAL(&critSectionMux);
    #define _enter_critical_isr() taskENTER_CRITICAL_FROM_ISR();
    #define _exit_critical_isr(x) taskEXIT_CRITICAL_FROM_ISR(x);
#else
    #define _enter_critical()   taskENTER_CRITICAL();
    #define _exit_critical()    taskEXIT_CRITICAL();
    #define _enter_critical_isr() taskENTER_CRITICAL_FROM_ISR();
    #define _exit_critical_isr(x) taskEXIT_CRITICAL_FROM_ISR(x);
#endif

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_result_t lv_thread_init(lv_thread_t * pxThread,  const char * const name,
                           lv_thread_prio_t xSchedPriority,
                           void (*pvStartRoutine)(void *), size_t usStackSize,
                           void * xAttr)
{
    pxThread->pTaskArg = xAttr;
    pxThread->pvStartRoutine = pvStartRoutine;

    BaseType_t xTaskCreateStatus;

    /* Allocate the task stack in internal SRAM (xTaskCreate always uses
     * internal RAM for dynamically created stacks).  A FreeRTOS task stack
     * must NOT live in PSRAM on ESP32 — see the file header for details. */
    xTaskCreateStatus = xTaskCreate(
                            prvRunThread,
                            name,
                            (configSTACK_DEPTH_TYPE)(usStackSize / sizeof(StackType_t)),
                            (void *)pxThread,
                            tskIDLE_PRIORITY + xSchedPriority,
                            &pxThread->xTaskHandle);

    /* Ensure that the FreeRTOS task was successfully created. */
    if(xTaskCreateStatus != pdPASS) {
        LV_LOG_ERROR("xTaskCreate failed!");
        return LV_RESULT_INVALID;
    }

    return LV_RESULT_OK;
}

lv_result_t lv_thread_delete(lv_thread_t * pxThread)
{
    /* Stack was allocated by xTaskCreate(), so free it with plain vTaskDelete(). */
    vTaskDelete(pxThread->xTaskHandle);

    return LV_RESULT_OK;
}

lv_result_t lv_mutex_init(lv_mutex_t * pxMutex)
{
    /* If mutex in uninitialized, perform initialization. */
    prvCheckMutexInit(pxMutex);

    return LV_RESULT_OK;
}

lv_result_t lv_mutex_lock(lv_mutex_t * pxMutex)
{
    /* If mutex in uninitialized, perform initialization. */
    prvCheckMutexInit(pxMutex);

    BaseType_t xMutexTakeStatus = xSemaphoreTakeRecursive(pxMutex->xMutex, portMAX_DELAY);
    if(xMutexTakeStatus != pdTRUE) {
        LV_LOG_ERROR("xSemaphoreTake failed!");
        return LV_RESULT_INVALID;
    }

    return LV_RESULT_OK;
}

lv_result_t lv_mutex_lock_isr(lv_mutex_t * pxMutex)
{
    /* If mutex in uninitialized, perform initialization. */
    prvCheckMutexInit(pxMutex);

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    BaseType_t xMutexTakeStatus = xSemaphoreTakeFromISR(pxMutex->xMutex, &xHigherPriorityTaskWoken);
    if(xMutexTakeStatus != pdTRUE) {
        LV_LOG_ERROR("xSemaphoreTake failed!");
        return LV_RESULT_INVALID;
    }

    /* If xHigherPriorityTaskWoken is now set to pdTRUE then a context switch
    should be performed to ensure the interrupt returns directly to the highest
    priority task.  The macro used for this purpose is dependent on the port in
    use and may be called portEND_SWITCHING_ISR(). */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

    return LV_RESULT_OK;
}

lv_result_t lv_mutex_unlock(lv_mutex_t * pxMutex)
{
    /* If mutex in uninitialized, perform initialization. */
    prvCheckMutexInit(pxMutex);

    BaseType_t xMutexGiveStatus = xSemaphoreGiveRecursive(pxMutex->xMutex);
    if(xMutexGiveStatus != pdTRUE) {
        LV_LOG_ERROR("xSemaphoreGive failed!");
        return LV_RESULT_INVALID;
    }

    return LV_RESULT_OK;
}

lv_result_t lv_mutex_delete(lv_mutex_t * pxMutex)
{
    if(pxMutex->xIsInitialized == pdFALSE)
        return LV_RESULT_INVALID;
    vSemaphoreDelete(pxMutex->xMutex);
    pxMutex->xIsInitialized = pdFALSE;

    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_init(lv_thread_sync_t * pxCond)
{
    /* If the cond is uninitialized, perform initialization. */
    prvCheckCondInit(pxCond);

    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_wait(lv_thread_sync_t * pxCond)
{
    lv_result_t lvRes = LV_RESULT_OK;

    /* If the cond is uninitialized, perform initialization. */
    prvCheckCondInit(pxCond);

#if LV_USE_FREERTOS_TASK_NOTIFY
    TaskHandle_t xCurrentTaskHandle = xTaskGetCurrentTaskHandle();

    _enter_critical();
    BaseType_t xSyncSygnal = pxCond->xSyncSignal;
    pxCond->xSyncSignal = pdFALSE;
    if(xSyncSygnal == pdFALSE) {
        /* The signal hasn't been sent yet. Tell the sender to notify this task */
        pxCond->xTaskToNotify = xCurrentTaskHandle;
    }
    /* If we have a signal from the other task, we should not ask to be notified */
    _exit_critical();

    if(xSyncSygnal == pdFALSE) {
        /* Wait for other task to notify this task. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    /* If the signal was received, no wait needs to be done */
#else
    uint32_t ulLocalWaitingThreads;

    /* Acquire the mutex. */
    xSemaphoreTake(pxCond->xSyncMutex, portMAX_DELAY);

    while(!pxCond->xSyncSignal) {
        /* Increase the counter of threads blocking on condition variable, then
         * release the mutex. */

        /* Atomically increments thread waiting by 1, and
         * stores number of threads waiting before increment. */
        ulLocalWaitingThreads = Atomic_Increment_u32(&pxCond->ulWaitingThreads);

        BaseType_t xMutexStatus = xSemaphoreGive(pxCond->xSyncMutex);

        /* Wait on the condition variable. */
        if(xMutexStatus == pdTRUE) {
            BaseType_t xCondWaitStatus = xSemaphoreTake(
                                             pxCond->xCondWaitSemaphore,
                                             portMAX_DELAY);

            /* Relock the mutex. */
            xSemaphoreTake(pxCond->xSyncMutex, portMAX_DELAY);

            if(xCondWaitStatus != pdTRUE) {
                LV_LOG_ERROR("xSemaphoreTake(xCondWaitSemaphore) failed!");
                lvRes = LV_RESULT_INVALID;

                /* Atomically decrements thread waiting by 1.
                 * If iLocalWaitingThreads is updated by other thread(s) in between,
                 * this implementation guarantees to decrement by 1 based on the
                 * value currently in pxCond->ulWaitingThreads. */
                prvTestAndDecrement(pxCond, ulLocalWaitingThreads + 1);
            }
        }
        else {
            LV_LOG_ERROR("xSemaphoreGive(xSyncMutex) failed!");
            lvRes = LV_RESULT_INVALID;

            /* Atomically decrements thread waiting by 1.
             * If iLocalWaitingThreads is updated by other thread(s) in between,
             * this implementation guarantees to decrement by 1 based on the
             * value currently in pxCond->ulWaitingThreads. */
            prvTestAndDecrement(pxCond, ulLocalWaitingThreads + 1);
        }
    }

    pxCond->xSyncSignal = pdFALSE;

    /* Release the mutex. */
    xSemaphoreGive(pxCond->xSyncMutex);
#endif

    return lvRes;
}

lv_result_t lv_thread_sync_signal(lv_thread_sync_t * pxCond)
{
    /* If the cond is uninitialized, perform initialization. */
    prvCheckCondInit(pxCond);

#if LV_USE_FREERTOS_TASK_NOTIFY
    _enter_critical();
    TaskHandle_t xTaskToNotify = pxCond->xTaskToNotify;
    pxCond->xTaskToNotify = NULL;
    if(xTaskToNotify == NULL) {
        /* No task waiting to be notified. Send this signal for later */
        pxCond->xSyncSignal = pdTRUE;
    }
    /* If a task is already waiting, there is no need to set the sync signal */
    _exit_critical();

    if(xTaskToNotify != NULL) {
        /* There is a task waiting. Send a notification to it */
        xTaskNotifyGive(xTaskToNotify);
    }
    /* If there was no task waiting to be notified, we sent a signal for it to see later. */
#else
    /* Acquire the mutex. */
    xSemaphoreTake(pxCond->xSyncMutex, portMAX_DELAY);

    pxCond->xSyncSignal = pdTRUE;

    /* Local copy of number of threads waiting. */
    uint32_t ulLocalWaitingThreads = pxCond->ulWaitingThreads;

    /* Test local copy of threads waiting is larger than zero. */
    while(ulLocalWaitingThreads > 0) {
        /* Atomically check whether the copy in memory has changed.
         * If not, set the copy of threads waiting in memory to zero. */
        if(ATOMIC_COMPARE_AND_SWAP_SUCCESS == Atomic_CompareAndSwap_u32(
               &pxCond->ulWaitingThreads,
               0,
               ulLocalWaitingThreads)) {
            /* Unblock all. */
            for(uint32_t i = 0; i < ulLocalWaitingThreads; i++) {
                xSemaphoreGive(pxCond->xCondWaitSemaphore);
            }

            break;
        }

        /* Local copy is out dated. Reload from memory and retry. */
        ulLocalWaitingThreads = pxCond->ulWaitingThreads;
    }

    /* Release the mutex. */
    xSemaphoreGive(pxCond->xSyncMutex);
#endif

    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_delete(lv_thread_sync_t * pxCond)
{
#if !LV_USE_FREERTOS_TASK_NOTIFY
    /* Cleanup all resources used by the cond. */
    vSemaphoreDelete(pxCond->xCondWaitSemaphore);
    vSemaphoreDelete(pxCond->xSyncMutex);
    pxCond->ulWaitingThreads = 0;
#endif
    pxCond->xSyncSignal = pdFALSE;
    pxCond->xIsInitialized = pdFALSE;

    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_signal_isr(lv_thread_sync_t * pxCond)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* If the cond is uninitialized, perform initialization. */
    prvCheckCondInitIsr(pxCond);

#if LV_USE_FREERTOS_TASK_NOTIFY
    uint32_t mask = _enter_critical_isr();
    TaskHandle_t xTaskToNotify = pxCond->xTaskToNotify;
    pxCond->xTaskToNotify = NULL;
    if(xTaskToNotify == NULL) {
        /* No task waiting to be notified. Send this signal for later */
        pxCond->xSyncSignal = pdTRUE;
    }
    /* If a task is already waiting, there is no need to set the sync signal */
    _exit_critical_isr(mask);

    if(xTaskToNotify != NULL) {
        /* There is a task waiting. Send a notification to it */
        vTaskNotifyGiveFromISR(xTaskToNotify, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    /* If there was no task waiting to be notified, we sent a signal for it to see later. */
#else
    /* Enter critical section to prevent preemption. */
    uint32_t mask = _enter_critical_isr();

    pxCond->xSyncSignal = pdTRUE;
    BaseType_t xAnyHigherPriorityTaskWoken = pdFALSE;

    /* Unblock all. */
    for(uint32_t i = 0; i < pxCond->ulWaitingThreads; i++) {
        xSemaphoreGiveFromISR(pxCond->xCondWaitSemaphore, &xAnyHigherPriorityTaskWoken);
        xHigherPriorityTaskWoken |= xAnyHigherPriorityTaskWoken;
    }

    _exit_critical_isr(mask);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
#endif

    return LV_RESULT_OK;
}


void lv_freertos_task_switch_in(const char * name)
{
    if(lv_strcmp(name, "IDLE")) globals->freertos_idle_task_running = false;
    else globals->freertos_idle_task_running = true;

    globals->freertos_task_switch_timestamp = lv_tick_get();
}

void lv_freertos_task_switch_out(void)
{
    uint32_t elaps = lv_tick_elaps(globals->freertos_task_switch_timestamp);
    if(globals->freertos_idle_task_running) globals->freertos_idle_time_sum += elaps;
    else globals->freertos_non_idle_time_sum += elaps;
}

uint32_t lv_os_get_idle_percent(void)
{
    if(globals->freertos_non_idle_time_sum + globals->freertos_idle_time_sum == 0) {
        LV_LOG_WARN("Not enough time elapsed to provide idle percentage");
        return 0;
    }

    uint32_t pct = (globals->freertos_idle_time_sum * 100) / (globals->freertos_idle_time_sum +
                                                              globals->freertos_non_idle_time_sum);

    globals->freertos_non_idle_time_sum = 0;
    globals->freertos_idle_time_sum = 0;

    return pct;
}

void lv_sleep_ms(uint32_t ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void prvRunThread(void * pxArg)
{
    lv_thread_t * pxThread = (lv_thread_t *)pxArg;

    /* Run the thread routine. */
    pxThread->pvStartRoutine((void *)pxThread->pTaskArg);

    vTaskDelete(NULL);
}

static void prvMutexInit(lv_mutex_t * pxMutex)
{
    pxMutex->xMutex = xSemaphoreCreateRecursiveMutex();

    /* Ensure that the FreeRTOS mutex was successfully created. */
    if(pxMutex->xMutex == NULL) {
        LV_LOG_ERROR("xSemaphoreCreateMutex failed!");
        return;
    }

    /* Mutex successfully created. */
    pxMutex->xIsInitialized = pdTRUE;
}

static void prvCheckMutexInit(lv_mutex_t * pxMutex)
{
    /* Check if the mutex needs to be initialized. */
    if(pxMutex->xIsInitialized == pdFALSE) {
        /* Mutex initialization must be in a critical section to prevent two threads
         * from initializing it at the same time. */
        _enter_critical();

        /* Check again that the mutex is still uninitialized, i.e. it wasn't
         * initialized while this function was waiting to enter the critical
         * section. */
        if(pxMutex->xIsInitialized == pdFALSE) {
            prvMutexInit(pxMutex);
        }

        /* Exit the critical section. */
        _exit_critical();
    }
}

static void prvCondInit(lv_thread_sync_t * pxCond)
{
    pxCond->xIsInitialized = pdTRUE;
    pxCond->xSyncSignal = pdFALSE;

#if LV_USE_FREERTOS_TASK_NOTIFY
    pxCond->xTaskToNotify = NULL;
#else
    pxCond->xCondWaitSemaphore = xSemaphoreCreateCounting(ulMAX_COUNT, 0U);

    /* Ensure that the FreeRTOS semaphore was successfully created. */
    if(pxCond->xCondWaitSemaphore == NULL) {
        LV_LOG_ERROR("xSemaphoreCreateCounting failed!");
        return;
    }

    pxCond->xSyncMutex = xSemaphoreCreateMutex();

    /* Ensure that the FreeRTOS mutex was successfully created. */
    if(pxCond->xSyncMutex == NULL) {
        LV_LOG_ERROR("xSemaphoreCreateMutex failed!");
        /* Cleanup. */
        vSemaphoreDelete(pxCond->xCondWaitSemaphore);
        return;
    }

    /* Condition variable successfully created. */
    pxCond->ulWaitingThreads = 0;
#endif
}

static void prvCheckCondInit(lv_thread_sync_t * pxCond)
{
    /* Check if the condition variable needs to be initialized. */
    if(pxCond->xIsInitialized == pdFALSE) {
        /* Cond initialization must be in a critical section to prevent two
         * threads from initializing it at the same time. */
        _enter_critical();

        /* Check again that the condition is still uninitialized, i.e. it wasn't
         * initialized while this function was waiting to enter the critical
         * section. */
        if(pxCond->xIsInitialized == pdFALSE) {
            prvCondInit(pxCond);
        }

        /* Exit the critical section. */
        _exit_critical();
    }
}

static void prvCheckCondInitIsr(lv_thread_sync_t * pxCond)
{
    /* Check if the condition variable needs to be initialized. */
    if(pxCond->xIsInitialized == pdFALSE) {
        /* Cond initialization must be in a critical section to prevent two
         * threads from initializing it at the same time. */
        uint32_t mask = _enter_critical_isr();

        /* Check again that the condition is still uninitialized, i.e. it wasn't
         * initialized while this function was waiting to enter the critical
         * section. */
        if(pxCond->xIsInitialized == pdFALSE) {
            prvCondInit(pxCond);
        }

        /* Exit the critical section. */
        _exit_critical_isr(mask);
    }
}

#if !LV_USE_FREERTOS_TASK_NOTIFY
static void prvTestAndDecrement(lv_thread_sync_t * pxCond,
                                uint32_t ulLocalWaitingThreads)
{
    /* Test local copy of threads waiting is larger than zero. */
    while(ulLocalWaitingThreads > 0) {
        /* Atomically check whether the copy in memory has changed.
         * If not, decrease the copy of threads waiting in memory. */
        if(ATOMIC_COMPARE_AND_SWAP_SUCCESS == Atomic_CompareAndSwap_u32(
               &pxCond->ulWaitingThreads,
               ulLocalWaitingThreads - 1,
               ulLocalWaitingThreads)) {
            /* Signal one succeeded. Break. */
            break;
        }

        /* Local copy may be out dated. Reload from memory and retry. */
        ulLocalWaitingThreads = pxCond->ulWaitingThreads;
    }
}
#endif

#endif /*LV_USE_OS == LV_OS_FREERTOS*/
