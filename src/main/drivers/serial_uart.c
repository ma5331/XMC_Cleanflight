/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Authors:
 * jflyper - Refactoring, cleanup and made pin-configurable
 * Dominic Clifton - Serial port abstraction, Separation of common STM32 code for cleanflight, various cleanups.
 * Hamasaki/Timecop - Initial baseflight code
*/

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

#include "build/build_config.h"

#include "common/utils.h"
#include "drivers/gpio.h"
#include "drivers/inverter.h"
#ifndef XMC4500_F100x1024
#include "drivers/rcc.h"
#endif

#include "drivers/serial.h"
#include "drivers/serial_uart.h"
#include "drivers/serial_uart_impl.h"

void uartSetBaudRate(serialPort_t *instance, uint32_t baudRate)
{
    uartPort_t *uartPort = (uartPort_t *)instance;
    uartPort->port.baudRate = baudRate;
    uartReconfigure(uartPort);
}

void uartSetMode(serialPort_t *instance, portMode_t mode)
{
    uartPort_t *uartPort = (uartPort_t *)instance;
    uartPort->port.mode = mode;
    uartReconfigure(uartPort);
}

void uartStartTxDMA(uartPort_t *s)
{
#ifndef XMC4500_F100x1024
#ifdef STM32F4
    DMA_Cmd(s->txDMAStream, DISABLE);
    DMA_MemoryTargetConfig(s->txDMAStream, (uint32_t)&s->port.txBuffer[s->port.txBufferTail], DMA_Memory_0);
    //s->txDMAStream->M0AR = (uint32_t)&s->port.txBuffer[s->port.txBufferTail];
    if (s->port.txBufferHead > s->port.txBufferTail) {
        s->txDMAStream->NDTR = s->port.txBufferHead - s->port.txBufferTail;
        s->port.txBufferTail = s->port.txBufferHead;
    }
    else {
        s->txDMAStream->NDTR = s->port.txBufferSize - s->port.txBufferTail;
        s->port.txBufferTail = 0;
    }
    s->txDMAEmpty = false;
    DMA_Cmd(s->txDMAStream, ENABLE);
#else
    s->txDMAChannel->CMAR = (uint32_t)&s->port.txBuffer[s->port.txBufferTail];
    if (s->port.txBufferHead > s->port.txBufferTail) {
        s->txDMAChannel->CNDTR = s->port.txBufferHead - s->port.txBufferTail;
        s->port.txBufferTail = s->port.txBufferHead;
    } else {
        s->txDMAChannel->CNDTR = s->port.txBufferSize - s->port.txBufferTail;
        s->port.txBufferTail = 0;
    }
    s->txDMAEmpty = false;
    DMA_Cmd(s->txDMAChannel, ENABLE);
#endif
#endif
}

uint32_t uartTotalRxBytesWaiting(const serialPort_t *instance)
{
    const uartPort_t *s = (const uartPort_t*)instance;

#ifndef XMC4500_F100x1024
#ifdef STM32F4
    if (s->rxDMAStream) {
        uint32_t rxDMAHead = s->rxDMAStream->NDTR;
#else
    if (s->rxDMAChannel) {
        uint32_t rxDMAHead = s->rxDMAChannel->CNDTR;
#endif
        if (rxDMAHead >= s->rxDMAPos) {
            return rxDMAHead - s->rxDMAPos;
        } else {
            return s->port.rxBufferSize + rxDMAHead - s->rxDMAPos;
        }
    }
#else
#endif

    if (s->port.rxBufferHead >= s->port.rxBufferTail) {
        return s->port.rxBufferHead - s->port.rxBufferTail;
    } else {
        return s->port.rxBufferSize + s->port.rxBufferHead - s->port.rxBufferTail;
    }
}

uint32_t uartTotalTxBytesFree(const serialPort_t *instance)
{
    const uartPort_t *s = (const uartPort_t*)instance;

    uint32_t bytesUsed;

    if (s->port.txBufferHead >= s->port.txBufferTail) {
        bytesUsed = s->port.txBufferHead - s->port.txBufferTail;
    } else {
        bytesUsed = s->port.txBufferSize + s->port.txBufferHead - s->port.txBufferTail;
    }

#ifndef XMC4500_F100x1024
#ifdef STM32F4
    if (s->txDMAStream) {
        /*
         * When we queue up a DMA request, we advance the Tx buffer tail before the transfer finishes, so we must add
         * the remaining size of that in-progress transfer here instead:
         */
        bytesUsed += s->txDMAStream->NDTR;
#else
    if (s->txDMAChannel) {
        /*
         * When we queue up a DMA request, we advance the Tx buffer tail before the transfer finishes, so we must add
         * the remaining size of that in-progress transfer here instead:
         */
        bytesUsed += s->txDMAChannel->CNDTR;
#endif
        /*
         * If the Tx buffer is being written to very quickly, we might have advanced the head into the buffer
         * space occupied by the current DMA transfer. In that case the "bytesUsed" total will actually end up larger
         * than the total Tx buffer size, because we'll end up transmitting the same buffer region twice. (So we'll be
         * transmitting a garbage mixture of old and new bytes).
         *
         * Be kind to callers and pretend like our buffer can only ever be 100% full.
         */
        if (bytesUsed >= s->port.txBufferSize - 1) {
            return 0;
        }
    }
#else
#endif
    return (s->port.txBufferSize - 1) - bytesUsed;
}

bool isUartTransmitBufferEmpty(const serialPort_t *instance)
{
    const uartPort_t *s = (const uartPort_t *)instance;

#ifdef STM32F4
    if (s->txDMAStream)
#else
    if (s->txDMAChannel)
#endif
        return s->txDMAEmpty;
    else
        return s->port.txBufferTail == s->port.txBufferHead;
}

uint8_t uartRead(serialPort_t *instance)
{
    uint8_t ch;
    uartPort_t *s = (uartPort_t *)instance;

#ifdef STM32F4
    if (s->rxDMAStream) {
#else
    if (s->rxDMAChannel) {
#endif
        ch = s->port.rxBuffer[s->port.rxBufferSize - s->rxDMAPos];
        if (--s->rxDMAPos == 0)
            s->rxDMAPos = s->port.rxBufferSize;
    } else {
        ch = s->port.rxBuffer[s->port.rxBufferTail];
        if (s->port.rxBufferTail + 1 >= s->port.rxBufferSize) {
            s->port.rxBufferTail = 0;
        } else {
            s->port.rxBufferTail++;
        }
    }
    return ch;
}

void uartWrite(serialPort_t *instance, uint8_t ch)
{
    uartPort_t *s = (uartPort_t *)instance;
    s->port.txBuffer[s->port.txBufferHead] = ch;
    if (s->port.txBufferHead + 1 >= s->port.txBufferSize) {
        s->port.txBufferHead = 0;
    } else {
        s->port.txBufferHead++;
    }

#ifndef XMC4500_F100x1024
#ifdef STM32F4
    if (s->txDMAStream) {
        if (!(s->txDMAStream->CR & 1))
#else
    if (s->txDMAChannel) {
        if (!(s->txDMAChannel->CCR & 1))
#endif
            uartStartTxDMA(s);
    } else {
        USART_ITConfig(s->USARTx, USART_IT_TXE, ENABLE);
    }
#else
    if (s->txDMAChannel)
    {

    }
    else
    {
    	uartDevice_t *uartDev = uartDevmap[instance->identifier];
		switch (uartDev->hardware->irqn_tx)
		{
			case USIC0_0_IRQn:
			case USIC1_0_IRQn:
			case USIC2_0_IRQn:
				XMC_USIC_CH_TriggerServiceRequest((XMC_USIC_CH_t*)s->USARTx, 0);
				break;
			case USIC0_1_IRQn:
			case USIC1_1_IRQn:
			case USIC2_1_IRQn:
				XMC_USIC_CH_TriggerServiceRequest((XMC_USIC_CH_t*)s->USARTx, 1);
				break;
			case USIC0_2_IRQn:
			case USIC1_2_IRQn:
			case USIC2_2_IRQn:
				XMC_USIC_CH_TriggerServiceRequest((XMC_USIC_CH_t*)s->USARTx, 2);
				break;
			case USIC0_3_IRQn:
			case USIC1_3_IRQn:
			case USIC2_3_IRQn:
				XMC_USIC_CH_TriggerServiceRequest((XMC_USIC_CH_t*)s->USARTx, 3);
				break;
			case USIC0_4_IRQn:
			case USIC1_4_IRQn:
			case USIC2_4_IRQn:
				XMC_USIC_CH_TriggerServiceRequest((XMC_USIC_CH_t*)s->USARTx, 4);
				break;
			case USIC0_5_IRQn:
			case USIC1_5_IRQn:
			case USIC2_5_IRQn:
				XMC_USIC_CH_TriggerServiceRequest((XMC_USIC_CH_t*)s->USARTx, 5);
				break;
		}
    }

#endif
}

const struct serialPortVTable uartVTable[] = {
    {
        .serialWrite = uartWrite,
        .serialTotalRxWaiting = uartTotalRxBytesWaiting,
        .serialTotalTxFree = uartTotalTxBytesFree,
        .serialRead = uartRead,
        .serialSetBaudRate = uartSetBaudRate,
        .isSerialTransmitBufferEmpty = isUartTransmitBufferEmpty,
        .setMode = uartSetMode,
        .writeBuf = NULL,
        .beginWrite = NULL,
        .endWrite = NULL,
    }
};

#ifdef USE_UART1
// USART1 Rx/Tx IRQ Handler
#ifdef XMC4500_F100x1024
#if UART1_USIC == U1C1
void USIC1_2_IRQHandler()
#else
void USIC0_0_IRQHandler()
#endif
{
	uartPort_t *s = &(uartDevmap[UARTDEV_1]->port);
	uartTxIrqHandler(s);
}

#if UART1_USIC == U1C1
void USIC1_3_IRQHandler()
#else
void USIC0_1_IRQHandler()
#endif
{
	uartPort_t *s = &(uartDevmap[UARTDEV_1]->port);
	uartRxIrqHandler(s);
}

#else
void USART1_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_1]->port);
    uartIrqHandler(s);
}
#endif
#endif

#ifdef USE_UART2
// USART2 Rx/Tx IRQ Handler
#ifdef XMC4500_F100x1024
void USIC1_0_IRQHandler()
{
	uartPort_t *s = &(uartDevmap[UARTDEV_2]->port);
	uartTxIrqHandler(s);
}

void USIC1_1_IRQHandler()
{
	uartPort_t *s = &(uartDevmap[UARTDEV_2]->port);
	uartRxIrqHandler(s);
}

#else
void USART2_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_2]->port);
    uartIrqHandler(s);
}
#endif
#endif

#ifdef USE_UART3
// USART3 Rx/Tx IRQ Handler
#ifdef XMC4500_F100x1024
#if UART3_USIC == U0C0
void USIC0_0_IRQHandler()
#else
void USIC2_2_IRQHandler()
#endif
{
	uartPort_t *s = &(uartDevmap[UARTDEV_3]->port);
	uartTxIrqHandler(s);
}

#if UART3_USIC == U0C0
void USIC0_1_IRQHandler()
#else
void USIC2_3_IRQHandler()
#endif
{
	uartPort_t *s = &(uartDevmap[UARTDEV_3]->port);
	uartRxIrqHandler(s);
}
#else
void USART3_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_3]->port);
    uartIrqHandler(s);
}
#endif
#endif

#ifdef USE_UART4
// UART4 Rx/Tx IRQ Handler
void UART4_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_4]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART5
// UART5 Rx/Tx IRQ Handler
void UART5_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_5]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART6
// USART6 Rx/Tx IRQ Handler
void USART6_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_6]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART7
// UART7 Rx/Tx IRQ Handler
void UART7_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_7]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART8
// UART8 Rx/Tx IRQ Handler
void UART8_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_8]->port);
    uartIrqHandler(s);
}
#endif
