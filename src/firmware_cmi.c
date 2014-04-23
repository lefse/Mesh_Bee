/*
 * firmware_cmi.c
 * Firmware for SeeedStudio Mesh Bee(Zigbee) module
 *
 * Copyright (c) NXP B.V. 2012.
 * Spread by SeeedStudio
 * Author     : Oliver Wang
 * Create Time: 2014/04
 * Change Log :
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/****************************************************************************/
/***        Include files                                                 ***/
/****************************************************************************/
#include <jendefs.h>
#include "firmware_cmi.h"
#include "firmware_uart.h"
#include "firmware_ringbuffer.h"
#include "common.h"
/****************************************************************************/
/***        Macro Definitions                                             ***/
/****************************************************************************/
#ifndef TRACE_CMI
#define TRACE_CMI TRUE
#endif

/****************************************************************************/
/***        Local Function Prototypes                                     ***/
/****************************************************************************/

/****************************************************************************/
/***        Exported Variables                                            ***/
/****************************************************************************/

/****************************************************************************/
/***        Local Functions                                               ***/
/****************************************************************************/



/****************************************************************************
 *
 * NAME: APP_isrUART1
 *
 * DESCRIPTION:
 * UART data server(UDS)
 * put received data into ringbuffer, or transfer data
 * Note: now we add communication interface layer,both input and output data
 *       were handled by CMI.
 *       (1)Master Mode: External[data]-->suli-->AUPS_rx_ringbuffer-->AUPS
 *                    AUPS[data]-->AUPS_tx_ringbuffer-->suli-->External
 *       If user want to send a AT command,Steps:
 *       1.Pack one calling pack_lib;
 *       2.Push it into rb_rx_uart.
 *       3.A command execute thread will pop rb_rx_uart and execute callback
 *         function periodically.
 *       (2)Slave Mode:
 *                    External[apiSpec Frame]--rb_rx_uart
 *       A command execute thread will pop rb_rx_uart and execute
 *       callback function periodically.
 ****************************************************************************/
OS_ISR(APP_isrUART1)
{
    uint8 intrpt;
    uint32 avlb_cnt;

    intrpt = (u8AHI_UartReadInterruptStatus(UART_COMM) >> 1) & 0x7;

    DBG_vPrintf(TRACE_CMI, "\r\nuart interrupt: %d \r\n", intrpt);
    if (intrpt == E_AHI_UART_INT_RXDATA)
    {
        avlb_cnt = u16AHI_UartReadRxFifoLevel(UART_COMM);

        if (avlb_cnt > 0)
        {
            uint8 tmp[RXFIFOLEN];

            /*
              anyhow we read to empty to clear interrupt flag
              if not do so, ISR will occur again and again
            */
            u16AHI_UartBlockReadData(UART_COMM, tmp, avlb_cnt);

            /* Push data into ringbuffer through CMI */
            CMI_vPushData(tmp, avlb_cnt);
        }
    }
    else if (intrpt == E_AHI_UART_INT_TX) //tx empty
    {
        uart_trigger_tx();
    }
}

/****************************************************************************
 *
 * NAME: CMI_vPushData
 *
 * DESCRIPTION:
 * Communication interface layer
 *
 * PARAMETERS: Name         RW  Usage
 *             data         R   Data from UART1
 *
 * RETURNS:
 * bool: TRUE - busy
 *
 ****************************************************************************/
void CMI_vPushData(void *data, int len)
{
    uint32 free_cnt = 0;    //free count
    uint32 min_cnt = 0;     //min count
	/* Master mode,data to AUPS ringbuffer */
#ifdef FW_MODE_MASTER
    if(E_MODE_MCU == g_sDevice.eMode)
    {
    	OS_eEnterCriticalSection(mutexRxRb);
    	free_cnt = ringbuffer_free_space(&rb_uart_aups);
    	OS_eExitCriticalSection(mutexRxRb);

    	min_cnt = MIN(free_cnt, len);
        DBG_vPrintf(TRACE_CMI, "rev_cnt: %u, free_cnt: %u \r\n", len, free_cnt);
    	/* If ringbuffer is full,don't push */
    	if(min_cnt > 0)
    	{
    	    OS_eEnterCriticalSection(mutexRxRb);
    	    ringbuffer_push(&rb_uart_aups, data, min_cnt);
    	    OS_eExitCriticalSection(mutexRxRb);
    	}
    }
    else  // AT/Data Mode, data-->rb_rx_uart
    {
        OS_eEnterCriticalSection(mutexRxRb);
        free_cnt = ringbuffer_free_space(&rb_rx_uart);
        OS_eExitCriticalSection(mutexRxRb);

        min_cnt = MIN(free_cnt, len);
        DBG_vPrintf(TRACE_CMI, "rev_cnt: %u, free_cnt: %u \r\n", len, free_cnt);
        if(min_cnt > 0)
        {
        	OS_eEnterCriticalSection(mutexRxRb);
        	ringbuffer_push(&rb_rx_uart, data, min_cnt);
        	uint32 size = ringbuffer_data_size(&rb_rx_uart);
        	OS_eExitCriticalSection(mutexRxRb);
        	/*
        	  the following mechanism is to improve the effective of every ZigBee packet frame
        	  by avoiding sending packet that is too short.
        	*/
        	if (size >= THRESHOLD_READ)
        	{
        		OS_eActivateTask(APP_taskHandleUartRx);             //Activate AT commands execution thread immediately
        	}
        	else
        	{
        		vResetATimer(APP_tmrHandleUartRx, APP_TIME_MS(5));  //Activate AT commands execution thread 5ms later
        	}
        }
    }

#else
    OS_eEnterCriticalSection(mutexRxRb);
    free_cnt = ringbuffer_free_space(&rb_rx_uart);
    OS_eExitCriticalSection(mutexRxRb);

    min_cnt = MIN(free_cnt, len);
    if(min_cnt > 0)
    {
    	OS_eEnterCriticalSection(mutexRxRb);
    	ringbuffer_push(&rb_rx_uart, data, min_cnt);
    	uint32 size = ringbuffer_data_size(&rb_rx_uart);
    	OS_eExitCriticalSection(mutexRxRb);
    	/*
    	  the following mechanism is to improve the effective of every ZigBee packet frame
    	  by avoiding sending packet that is too short.
    	*/
    	if (size >= THRESHOLD_READ)
    	{
    		OS_eActivateTask(APP_taskHandleUartRx);             //Activate AT commands execution thread immediately
    	}
    	else
    	{
    		vResetATimer(APP_tmrHandleUartRx, APP_TIME_MS(5));  //Activate AT commands execution thread 5ms later
    	}
    }
#endif
}

/****************************************************************************
 *
 * NAME: CMI_vTxData
 *
 * DESCRIPTION:
 * Communication interface layer
 *
 * PARAMETERS: Name         RW  Usage
 *             data         R   Data from UART1
 *
 * RETURNS:
 * bool: TRUE - busy
 *
 ****************************************************************************/
int CMI_vTxData(void *data, int len)
{
#ifdef FW_MODE_MASTER
	/* At Data mode,data send to UART1 directly */
	if(E_MODE_DATA == g_sDevice.eMode)
	{
		uart_tx_data(data, len);
	}
	else
	{
	    OS_eEnterCriticalSection(mutexAirPort);
        uint32 free_cnt = ringbuffer_free_space(&rb_air_aups);
	    OS_eExitCriticalSection(mutexAirPort);

	    /*
	      if free size < len, discard it,return ERR immediately,
	      waiting may result in a blocking of AUPS thread.
        */
	    if(free_cnt >= len)
	    {
		    OS_eEnterCriticalSection(mutexAirPort);
		    ringbuffer_push(&rb_air_aups, data, len);
		    OS_eExitCriticalSection(mutexAirPort);
	    }
	    else
	    {
            return false;
	    }
	}
#else
	/* Mechanism: wait until ringbuffer has enough space */
    uart_tx_data(data, len);
#endif
    return true;
}
