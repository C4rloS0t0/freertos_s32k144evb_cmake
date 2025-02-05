/* 
 * Copyright (c) 2015 - 2016 , Freescale Semiconductor, Inc.                             
 * Copyright 2016-2017 NXP                                                                    
 * All rights reserved.                                                                  
 *                                                                                       
 * THIS SOFTWARE IS PROVIDED BY NXP "AS IS" AND ANY EXPRESSED OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL NXP OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.                          
 */

/*
 * main-blinky.c is included when the "Blinky" build configuration is used.
 * main-full.c is included when the "Full" build configuration is used.
 *
 * main-blinky.c (this file) defines a very simple demo that creates two tasks,
 * one queue, and one timer.  It also demonstrates how Cortex-M4 interrupts can
 * interact with FreeRTOS tasks/timers.
 *
 * This simple demo project runs 'stand alone' (without the rest of the tower
 * system) on the Freedom Board or Validation Board, which is populated with a
 * S32K144 Cortex-M4 microcontroller.
 *
 * The idle hook function:
 * The idle hook function demonstrates how to query the amount of FreeRTOS heap
 * space that is remaining (see vApplicationIdleHook() defined in this file).
 *
 * The main() Function:
 * main() creates one software timer, one queue, and two tasks.  It then starts
 * the scheduler.
 *
 * The Queue Send Task:
 * The queue send task is implemented by the prvQueueSendTask() function in
 * this file.  prvQueueSendTask() sits in a loop that causes it to repeatedly
 * block for 200 milliseconds, before sending the value 100 to the queue that
 * was created within main().  Once the value is sent, the task loops back
 * around to block for another 200 milliseconds.
 *
 * The Queue Receive Task:
 * The queue receive task is implemented by the prvQueueReceiveTask() function
 * in this file.  prvQueueReceiveTask() sits in a loop that causes it to
 * repeatedly attempt to read data from the queue that was created within
 * main().  When data is received, the task checks the value of the data, and
 * if the value equals the expected 100, toggles the green LED.  The 'block
 * time' parameter passed to the queue receive function specifies that the task
 * should be held in the Blocked state indefinitely to wait for data to be
 * available on the queue.  The queue receive task will only leave the Blocked
 * state when the queue send task writes to the queue.  As the queue send task
 * writes to the queue every 200 milliseconds, the queue receive task leaves the
 * Blocked state every 200 milliseconds, and therefore toggles the blue LED
 * every 200 milliseconds.
 *
 * The LED Software Timer and the Button Interrupt:
 * The user button BTN1 is configured to generate an interrupt each time it is
 * pressed.  The interrupt service routine switches the red LED on, and
 * resets the LED software timer.  The LED timer has a 5000 millisecond (5
 * second) period, and uses a callback function that is defined to just turn the
 * LED off again.  Therefore, pressing the user button will turn the LED on, and
 * the LED will remain on until a full five seconds pass without the button
 * being pressed.
 */

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

/* SDK includes. */
#include "interrupt_manager.h"
#include "clock_manager.h"
#include "clockMan1.h"
#include "pin_mux.h"

#include "BoardDefines.h"

/* User def includes */
#include "LedControl.h"
#include "adc_app.h"
#include "uart_app.h"
#include "can_app.h"

/* Priorities at which the tasks are created. */
#define mainQUEUE_RECEIVE_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define mainQUEUE_SEND_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

/* The rate at which data is sent to the queue, specified in milliseconds, and
converted to ticks using the portTICK_PERIOD_MS constant. */
#define mainQUEUE_SEND_FREQUENCY_MS (200 / portTICK_PERIOD_MS)

/* The LED will remain on until the button has not been pushed for a full
5000ms. */
#define mainBUTTON_LED_TIMER_PERIOD_MS (5000UL / portTICK_PERIOD_MS)

/* The number of items the queue can hold.  This is 1 as the receive task
will remove items as they are added, meaning the send task should always find
the queue empty. */
#define mainQUEUE_LENGTH (1)

/* The LED toggle by the queue receive task (blue). */
#define mainTASK_CONTROLLED_LED (1UL << 0UL)

/* The LED turned on by the button interrupt, and turned off by the LED timer
(green). */
#define mainTIMER_CONTROLLED_LED (1UL << 1UL)

/* The vector used by the GPIO port C.  Button SW7 is configured to generate
an interrupt on this port. */
#define mainGPIO_C_VECTOR (61)

/*-----------------------------------------------------------*/

/*
 * Setup the NVIC, LED outputs, and button inputs.
 */
static void prvSetupHardware(void);

/*
 * The tasks as described in the comments at the top of this file.
 */
static void prvQueueReceiveTask(void *pvParameters);
static void prvQueueSendTask(void *pvParameters);

/*
 * The LED timer callback function.  This does nothing but switch off the
 * LED defined by the mainTIMER_CONTROLLED_LED constant.
 */
static void prvButtonLEDTimerCallback(TimerHandle_t xTimer);

/*-----------------------------------------------------------*/

/* The queue used by both tasks. */
static QueueHandle_t xQueue = NULL;
QueueHandle_t xLedCtrlSig = NULL;
QueueHandle_t xVolSig = NULL;

uint16 adcMax = 0u;

/* The LED software timer.  This uses prvButtonLEDTimerCallback() as its callback
function. */
static TimerHandle_t xButtonLEDTimer = NULL;

static uint8 debug_test = 0u;

#include "pins_driver.h"

void BoardInit(void)
{
    /* Initialize and configure clocks
     *  -   Setup system clocks, dividers
     *  -   see clock manager component for more details
     */
    CLOCK_SYS_Init(g_clockManConfigsArr, CLOCK_MANAGER_CONFIG_CNT,
                   g_clockManCallbacksArr, CLOCK_MANAGER_CALLBACK_CNT);
    CLOCK_SYS_UpdateConfiguration(0U, CLOCK_MANAGER_POLICY_AGREEMENT);

    /* Initialize pins
     *  -   See PinSettings component for more info
     */
    PINS_DRV_Init(NUM_OF_CONFIGURED_PINS, g_pin_mux_InitConfigArr);
    /* Configure ports */
    // PINS_DRV_SetMuxModeSel(LED_PORT, LED1, PORT_MUX_AS_GPIO); 
    // PINS_DRV_SetMuxModeSel(LED_PORT, LED2, PORT_MUX_AS_GPIO);
    // PINS_DRV_SetMuxModeSel(BTN_PORT, BTN_PIN, PORT_MUX_AS_GPIO);
}

void GPIOInit(void)
{
    /* Output direction for LEDs */
    PINS_DRV_SetPinsDirection(GPIO_PORT, (1 << LED1) | (1 << LED2));

    /* Set Output value LEDs */
    PINS_DRV_ClearPins(GPIO_PORT, 1 << LED2);

    /* Start with LEDs off. */
    PINS_DRV_SetPins(LED_GPIO, (1 << LED1) | (1 << LED2));

    /* Setup button pin */
    PINS_DRV_SetPinsDirection(BTN_GPIO, ~((1 << BTN1_PIN)|(1 << BTN2_PIN)));

    /* Setup button pins interrupt */
    PINS_DRV_SetPinIntSel(BTN_PORT, BTN1_PIN, PORT_INT_RISING_EDGE);
    PINS_DRV_SetPinIntSel(BTN_PORT, BTN2_PIN, PORT_INT_RISING_EDGE);

    /* Install Button interrupt handler */
    INT_SYS_InstallHandler(BTN_PORT_IRQn, vPort_C_ISRHandler, (isr_t *)NULL);
    /* Enable Button interrupt handler */
    INT_SYS_EnableIRQ(BTN_PORT_IRQn);

    /* The interrupt calls an interrupt safe API function - so its priority must
    be equal to or lower than configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY. */
    INT_SYS_SetPriority(BTN_PORT_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
}

void ADCInit(void)
{
    adc_resolution_t resolution;
    status_t status;

    resolution = ((extension_adc_s32k1xx_t *)(adc_pal1_InitConfig0.extension))->resolution;

    if (resolution == ADC_RESOLUTION_8BIT)
    {
        adcMax = (uint16_t)(1 << 8);
    }
    else if (resolution == ADC_RESOLUTION_10BIT)
    {
        adcMax = (uint16_t)(1 << 10);
    }
    else
    {
        adcMax = (uint16_t)(1 << 12);
    }

    /* Initialize the ADC PAL
     *  -   See ADC PAL component for the configuration details
     */
    for (int i = 0; i < 4; i++)
    {
        DEV_ASSERT(adc_pal1_ChansArray00[i] == ADC_CHN);
    }
    for (int i = 0; i < 5; i++)
    {
        DEV_ASSERT(adc_pal1_ChansArray02[i] == ADC_CHN);
    }
    DEV_ASSERT(adc_pal1_instance.instIdx == ADC_INSTANCE);
    status = ADC_Init(&adc_pal1_instance, &adc_pal1_InitConfig0);
    DEV_ASSERT(status == STATUS_SUCCESS);

    /* Send welcome message */
    /* Start the selected SW triggered group of conversions */
    status = ADC_StartGroupConversion(&adc_pal1_instance, selectedGroupIndex);
    DEV_ASSERT(status == STATUS_SUCCESS);
}

/*-----------------------------------------------------------*/

void rtos_start(void)
{
    /* Configure the NVIC, LED outputs and button inputs. */
    prvSetupHardware();

    /* Create the queue. */
    xQueue = xQueueCreate(mainQUEUE_LENGTH, sizeof(unsigned long));

    /* Creat led control sig queue. */
    xLedCtrlSig = xQueueCreate(mainQUEUE_LENGTH, sizeof(uint8));
    /* voltage signal from adc . */
    xVolSig = xQueueCreate(mainQUEUE_LENGTH, sizeof(float32));

    if (xQueue != NULL)
    {
        /* Start the two tasks as described in the comments at the top of this
        file. */

        xTaskCreate(prvQueueReceiveTask, "RX", configMINIMAL_STACK_SIZE, NULL, mainQUEUE_RECEIVE_TASK_PRIORITY, NULL);
        xTaskCreate(prvQueueSendTask, "TX", configMINIMAL_STACK_SIZE, NULL, mainQUEUE_RECEIVE_TASK_PRIORITY, NULL);

        /* User app create */
        xTaskCreate(vLedControl, "LedControl", configMINIMAL_STACK_SIZE, NULL, mainQUEUE_RECEIVE_TASK_PRIORITY, NULL);

        xTaskCreate(vAdcApp, "ADC_Voltage_Calculate", TASK_ADC_STACK_SIZE, NULL, mainQUEUE_SEND_TASK_PRIORITY, NULL);

        xTaskCreate(vCanApp, "CAN_Communication", TASK_CAN_STACK_SIZE, NULL, mainQUEUE_SEND_TASK_PRIORITY, NULL);

        /* Create the software timer that is responsible for turning off the LED
        if the button is not pushed within 5000ms, as described at the top of
        this file. */
        xButtonLEDTimer = xTimerCreate("ButtonLEDTimer",               /* A text name, purely to help debugging. */
                                       mainBUTTON_LED_TIMER_PERIOD_MS, /* The timer period, in this case 5000ms (5s). */
                                       pdFALSE,                        /* This is a one shot timer, so xAutoReload is set to pdFALSE. */
                                       (void *)0,                      /* The ID is not used, so can be set to anything. */
                                       prvButtonLEDTimerCallback       /* The callback function that switches the LED off. */
        );

        /* Start the tasks and timer running. */
        vTaskStartScheduler();
    }

    /* If all is well, the scheduler will now be running, and the following line
    will never be reached.  If the following line does execute, then there was
    insufficient FreeRTOS heap memory available for the idle and/or timer tasks
    to be created.  See the memory management section on the FreeRTOS web site
    for more details. */
    for (;;)
        ;
}
/*-----------------------------------------------------------*/

static void prvButtonLEDTimerCallback(TimerHandle_t xTimer)
{
    /* Casting xTimer to void because it is unused */
    (void)xTimer;

    /* The timer has expired - so no button pushes have occurred in the last
    five seconds - turn the LED off. */
    PINS_DRV_SetPins(LED_GPIO, (1 << LED2));
}
/*-----------------------------------------------------------*/

/* The ISR executed when the user button is pushed. */
void vPort_C_ISRHandler(void)
{
    portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;

    /* The button was pushed, so ensure the LED is on before resetting the
    LED timer.  The LED timer will turn the LED off if the button is not
    pushed within 5000ms. */
    PINS_DRV_ClearPins(LED_GPIO, (1 << LED2));
    /* This interrupt safe FreeRTOS function can be called from this interrupt
    because the interrupt priority is below the
    configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY setting in FreeRTOSConfig.h. */
    xTimerResetFromISR(xButtonLEDTimer, &xHigherPriorityTaskWoken);

    /* Clear the interrupt before leaving. */
    PINS_DRV_ClearPortIntFlagCmd(BTN_PORT);

    /* If calling xTimerResetFromISR() caused a task (in this case the timer
    service/daemon task) to unblock, and the unblocked task has a priority
    higher than or equal to the task that was interrupted, then
    xHigherPriorityTaskWoken will now be set to pdTRUE, and calling
    portEND_SWITCHING_ISR() will ensure the unblocked task runs next. */
    portEND_SWITCHING_ISR(xHigherPriorityTaskWoken);
}
/*-----------------------------------------------------------*/

static void prvQueueSendTask(void *pvParameters)
{
    TickType_t xNextWakeTime;
    unsigned long ulValueToSend = 0UL;

    /* Casting pvParameters to void because it is unused */
    (void)pvParameters;

    /* Initialise xNextWakeTime - this only needs to be done once. */
    xNextWakeTime = xTaskGetTickCount();

    for (;;)
    {
        // print("Thread - prvQueueSendTask Run - 100ms\r\n");
        /* Place this task in the blocked state until it is time to run again.
        The block time is specified in ticks, the constant used converts ticks
        to ms.  While in the Blocked state this task will not consume any CPU
        time. */
        if (200UL == ulValueToSend)
        {
            ulValueToSend = 201UL;
        }
        else if (201UL == ulValueToSend)
        {
            ulValueToSend = 200UL;
        }

        /* Send to the queue - causing the queue receive task to unblock and
        toggle an LED.  0 is used as the block time so the sending operation
        will not block - it shouldn't need to block as the queue should always
        be empty at this point in the code. */
        xQueueSend(xQueue, &ulValueToSend, mainDONT_BLOCK);
        vTaskDelayUntil(&xNextWakeTime, TASK_PERIOD_100_MS);
    }
}
/*-----------------------------------------------------------*/

static void prvQueueReceiveTask(void *pvParameters)
{
    unsigned long ulReceivedValue;

    /* Casting pvParameters to void because it is unused */
    (void)pvParameters;

    for (;;)
    {
        // print("Thread - prvQueueReceiveTask Run - 1ms\r\n");
        /* Wait until something arrives in the queue - this task will block
        indefinitely provided INCLUDE_vTaskSuspend is set to 1 in
        FreeRTOSConfig.h. */
        xQueueReceive(xQueue, &ulReceivedValue, portMAX_DELAY);
        debug_test = 0u;
        /*  To get here something must have been received from the queue, but
        is it the expected value?  If it is, toggle the LED. */
        if (ulReceivedValue == 200UL)
        {
            PINS_DRV_ClearPins(LED_GPIO, (1 << LED2));
        }
        else if (ulReceivedValue == 201UL)
        {
            PINS_DRV_SetPins(LED_GPIO, (1 << LED2));
        }
        else
        {
            /* Debug info*/
            debug_test = 1u;
        }
    }
}
/*-----------------------------------------------------------*/

static void prvSetupHardware(void)
{
    status_t status;
    
    BoardInit();
    GPIOInit();
    ADCInit();

    /* Initialize LPUART instance
     *  -   See LPUART component for configuration details
     * If the initialization failed, trigger an hardware breakpoint
     */
    status = LPUART_DRV_Init(INST_LPUART1, &lpuart1_State, &lpuart1_InitConfig0);
    DEV_ASSERT(status == STATUS_SUCCESS);

    /* Initial CAN */
    CAN_Init(&can_pal1_instance, &can_pal1_Config0);
    print(initOKStr);
}
/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook(void)
{
    /* Called if a call to pvPortMalloc() fails because there is insufficient
    free memory available in the FreeRTOS heap.  pvPortMalloc() is called
    internally by FreeRTOS API functions that create tasks, queues, software
    timers, and semaphores.  The size of the FreeRTOS heap is set by the
    configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
    taskDISABLE_INTERRUPTS();
    for (;;)
        ;
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    (void)pcTaskName;
    (void)pxTask;

    /* Run time stack overflow checking is performed if
    configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
    function is called if a stack overflow is detected. */
    taskDISABLE_INTERRUPTS();
    for (;;)
        ;
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook(void)
{
    volatile size_t xFreeHeapSpace;

    /* This function is called on each cycle of the idle task.  In this case it
    does nothing useful, other than report the amount of FreeRTOS heap that
    remains unallocated. */
    xFreeHeapSpace = xPortGetFreeHeapSize();

    if (xFreeHeapSpace > 100)
    {
        /* By now, the kernel has allocated everything it is going to, so
        if there is a lot of heap remaining unallocated then
        the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
        reduced accordingly. */
    }
}
/*-----------------------------------------------------------*/

/* The Blinky build configuration does not include run time stats gathering,
however, the Full and Blinky build configurations share a FreeRTOSConfig.h
file.  Therefore, dummy run time stats functions need to be defined to keep the
linker happy. */
void vMainConfigureTimerForRunTimeStats(void) {}
unsigned long ulMainGetRunTimeCounterValue(void) { return 0UL; }

/* A tick hook is used by the "Full" build configuration.  The Full and blinky
build configurations share a FreeRTOSConfig.h header file, so this simple build
configuration also has to define a tick hook - even though it does not actually
use it for anything. */
void vApplicationTickHook(void) {}

/*-----------------------------------------------------------*/
