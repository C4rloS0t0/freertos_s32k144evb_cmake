/**
 *-----------------------------------------------------------------------------
 * @file adc_app.c
 * @brief 
 * @author shibo jiang
 * @version 0.0.0.1
 * @date 2021-07-13
 * @note [change history] 
 * 
 * @copyright NAAA_
 *-----------------------------------------------------------------------------
 */
#include "adc_app.h"

/* Flag used to store if an ADC PAL conversion group has finished executing */
volatile boolean groupConvDone = false;
/* Flag used to store the offset of the most recent result in the result buffer */
volatile uint32 resultLastOffset = 0;

/* Variable to store value from ADC conversion */
volatile uint16 adcRawValue;

/**
 *-----------------------------------------------------------------------------
 * @brief 
 * 
 * @param callbackInfo 
 * @param userData 
 *-----------------------------------------------------------------------------
 */
void adc_pal1_callback00(const adc_callback_info_t * const callbackInfo, 
                        void * userData)
{
    (void) userData;

    groupConvDone = true;
    resultLastOffset = callbackInfo->resultBufferTail;
}

/**
 *-----------------------------------------------------------------------------
 * @brief 
 * 
 * @param callbackInfo 
 * @param userData 
 *-----------------------------------------------------------------------------
 */
void adc_pal1_callback02(const adc_callback_info_t * const callbackInfo, void * userData)
{
    (void) userData;

    groupConvDone = true;
    resultLastOffset = callbackInfo->resultBufferTail;
}

void vAdcApp (void *pvParameters)
{
    status_t status;
    uint16 resultStartOffset;
    uint32 sum, avg;
    float32 avgVolts;
    char msg[255] = { 0, };
    TickType_t xNextWakeTime;

    /* Casting pvParameters to void because it is unused */
    (void)pvParameters;
    xNextWakeTime = xTaskGetTickCount();

    /* Select the index of a SW triggered group of conversions (see ADC PAL component) */
    uint8 selectedGroupIndex = 0u; 

    for( ;; )
    {
        /* Wait until something arrives in the queue - this task will block
        indefinitely provided INCLUDE_vTaskSuspend is set to 1 in
        FreeRTOSConfig.h. */

        /* Start the selected SW triggered group of conversions */
        status = ADC_StartGroupConversion(&adc_pal1_instance, selectedGroupIndex);
        DEV_ASSERT(status == STATUS_SUCCESS);

        /* Called only for demonstration purpose - it is not necessary and doesn't influence application functionality. */
        status = ADC_StartGroupConversion(&adc_pal1_instance, 1u); /* Starting another SW triggered group while other is running will return BUSY. */
        /* When running step by step, it is expected that this DEV_ASSERT fails - because the group started by the first ADC_StartGroupConversion()
        finishes execution before the second call, so the current status is SUCCESS instead of BUSY. */
        DEV_ASSERT(status == STATUS_BUSY);
        /* Called only for demonstration purpose - it is not necessary and doesn't influence application functionality. */
        status = ADC_EnableHardwareTrigger(&adc_pal1_instance, 2u); /* Enabling another HW triggered group while other SW triggered is running will return BUSY. */
        /* When running step by step, it is expected that this DEV_ASSERT fails - because the group started by the first ADC_StartGroupConversion()
        finishes execution before calling ADC_EnableHardwareTrigger(), so the current status is SUCCESS instead of BUSY. */
        DEV_ASSERT(status == STATUS_BUSY);

        uint8_t iter = 0;
        uint8_t numChans = adc_pal1_InitConfig0.groupConfigArray[selectedGroupIndex].numChannels;
        resultStartOffset = 0u;
        while(iter < NUM_CONV_GROUP_ITERATIONS)
        {
            /* Wait for group to finish */
            if(groupConvDone == true)
            {
                /* Calculate average value of the results in the group of conversions */
                sum = 0;
                for(uint8_t idx = resultStartOffset; idx <= resultLastOffset; idx++)
                {
                    sum += adc_pal1_Results00[idx]; /* Results are directly available in resultBuffer associated with the group at initialization */
                }
                DEV_ASSERT((resultLastOffset - resultStartOffset + 1) == numChans);
                avg = sum / numChans;

                /* Convert avg to volts */
                avgVolts = ((float) avg / adcMax) * (ADC_VREFH - ADC_VREFL);
                /* Convert avg to string */
                floatToStr(&avgVolts, msg, 5);

                /* Send the result to the user via LPUART */
                print(headerStr);
                print(msg);
                print(" V\r\n");

                /* Reset flag for group conversion status */
                groupConvDone = false;
                iter ++;

                OSIF_TimeDelay(DELAY_BETWEEN_SW_TRIG_GROUPS);

                /* Restart the SW triggered group of conversions */
                status = ADC_StartGroupConversion(&adc_pal1_instance, selectedGroupIndex); /* Restart can be avoided if SW triggered group is configured to run in continuous mode */
                DEV_ASSERT(status == STATUS_SUCCESS);
            }
        }
        /* Stop the extra SW triggered conversion */
        status = ADC_StopGroupConversion(&adc_pal1_instance, selectedGroupIndex, 1 /* millisecond */);
        DEV_ASSERT(status == STATUS_SUCCESS);

        // status = ADC_Deinit(&adc_pal1_instance);
        // DEV_ASSERT(status == STATUS_SUCCESS);

        // status = LPUART_DRV_Deinit(INST_LPUART1);
        // DEV_ASSERT(status == STATUS_SUCCESS);

        vTaskDelayUntil( &xNextWakeTime, TASK_PERIOD_100_MS );
        xQueueSend( xVolSig, &avgVolts, mainDONT_BLOCK );
    }
}