/* Copyright (c) 2014, Nordic Semiconductor ASA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/** @file
@brief Implementation of the ACI transport layer module
*/

#include <SPI.h>
#include "hal_platform.h"
#include "hal_aci_tl.h"
#include <avr/sleep.h>

static void           m_print_aci_data(hal_aci_data_t *p_data);
static uint8_t        spi_readwrite(uint8_t aci_byte);

static hal_aci_data_t received_data;
static bool           aci_debug_print = false;

aci_queue_t    aci_tx_q;
aci_queue_t    aci_rx_q;

static aci_pins_t	 *a_pins_local_ptr;

static void m_aci_q_init(aci_queue_t *aci_q)
{
  uint8_t loop;
  
  aci_q->head = 0;
  aci_q->tail = 0;
  for(loop=0; loop<ACI_QUEUE_SIZE; loop++)
  {
    aci_q->aci_data[loop].buffer[0] = 0x00;
    aci_q->aci_data[loop].buffer[1] = 0x00;
  }
}

void hal_aci_debug_print(bool enable)
{
	aci_debug_print = enable;
}

bool m_aci_q_enqueue(aci_queue_t *aci_q, hal_aci_data_t *p_data)
{
  const uint8_t next = (aci_q->tail + 1) % ACI_QUEUE_SIZE;
  const uint8_t length = p_data->buffer[0];
  
  if (next == aci_q->head)
  {
    /* full queue */
    return false;
  }
  aci_q->aci_data[aci_q->tail].status_byte = 0;
  
  memcpy((uint8_t *)&(aci_q->aci_data[aci_q->tail].buffer[0]), (uint8_t *)&p_data->buffer[0], length + 1);
  aci_q->tail = next;
  
  return true;
}

//@comment after a port to a new mcu have test for the queue states, esp. the full and the empty states
static bool m_aci_q_dequeue(aci_queue_t *aci_q, hal_aci_data_t *p_data)
{
  if (aci_q->head == aci_q->tail)
  {
    /* empty queue */
    return false;
  }

  /* p_data might be NULL if function calling this wishes to discard the popped message */
  if (p_data !=NULL )
  {
    memcpy((uint8_t *)p_data, (uint8_t *)&(aci_q->aci_data[aci_q->head]), sizeof(hal_aci_data_t));
  }
  
  aci_q->head = (aci_q->head + 1) % ACI_QUEUE_SIZE;
  
  return true;
}

static bool m_aci_q_peek(aci_queue_t *aci_q, hal_aci_data_t *p_data)
{
  if (aci_q->head == aci_q->tail)
  {
    /* empty queue */
    return false;
  }

  memcpy((uint8_t *)p_data, (uint8_t *)&(aci_q->aci_data[aci_q->head]), sizeof(hal_aci_data_t));

  return true;
}

static bool m_aci_q_is_empty(aci_queue_t *aci_q)
{
  return (aci_q->head == aci_q->tail);
}

static bool m_aci_q_is_full(aci_queue_t *aci_q)
{
  uint8_t next;
  bool state;
  
  //This should be done in a critical section
  noInterrupts();
  next = (aci_q->tail + 1) % ACI_QUEUE_SIZE;  
  
  if (next == aci_q->head)
  {
    state = true;
  }
  else
  {
    state = false;
  }
  
  interrupts();
  //end
  
  return state;
}

void m_print_aci_data(hal_aci_data_t *p_data)
{
  const uint8_t length = p_data->buffer[0];
  uint8_t i;
  Serial.print(length, DEC);
  Serial.print(" :");
  for (i=0; i<=length; i++)
  {
    Serial.print(p_data->buffer[i], HEX);
    Serial.print(F(", "));
  }
  Serial.println(F(""));
}

void hal_aci_pin_reset(void)
{
    if (UNUSED != a_pins_local_ptr->reset_pin)
    {
        pinMode(a_pins_local_ptr->reset_pin, OUTPUT);

        if ((REDBEARLAB_SHIELD_V1_1     == a_pins_local_ptr->board_name) ||
            (REDBEARLAB_SHIELD_V2012_07 == a_pins_local_ptr->board_name))
        {
            //The reset for the Redbearlab v1.1 and v2012.07 boards are inverted and has a Power On Reset
            //circuit that takes about 100ms to trigger the reset
            digitalWrite(a_pins_local_ptr->reset_pin, 1);
            delay(100);
            digitalWrite(a_pins_local_ptr->reset_pin, 0);		
        }
        else
        {
            digitalWrite(a_pins_local_ptr->reset_pin, 1);
            digitalWrite(a_pins_local_ptr->reset_pin, 0);		
            digitalWrite(a_pins_local_ptr->reset_pin, 1);
        }
    }
}

static void m_rdy_line_handle(void)
{
  hal_aci_data_t *p_aci_data;
  
  if (a_pins_local_ptr->interface_is_interrupt)
  {
    detachInterrupt(a_pins_local_ptr->interrupt_number);
  }
  
  // Receive or transmit data
  p_aci_data = hal_aci_tl_poll_get();
  
  // Check if we received data
  if (p_aci_data->buffer[0] > 0)
  {
    if (!m_aci_q_enqueue(&aci_rx_q, p_aci_data))
    {
      /* Receive Buffer full.
         Should never happen.
         Spin in a while loop.
         */	  
       while(1);
    }
    if (m_aci_q_is_full(&aci_rx_q))
    {
      /* Disable RDY line interrupt.
         Will latch any pending RDY lines, so when enabled it again this
         routine should be taken again */
	  if (true == a_pins_local_ptr->interface_is_interrupt)
	  {
		EIMSK &= ~(0x2);
	  }
    }    
  }
}

/*
 Check the RDYN line. When the RDYN line goes low, run the SPI master and place the returned 
 ACI Event in the p_aci_evt_data
*/
static void m_aci_device_query(void)
{
  /*
  When the RDYN goes low it means the nRF8001 is ready for the SPI transaction
  */
  if (0 == digitalRead(a_pins_local_ptr->rdyn_pin))
  {
    /*
    Now process the Master SPI
    */
    m_rdy_line_handle();
    return;
  }
  
  /*
   RDYN line was not low
   When there are commands in the Command queue and the event queue has space for
   more events place the REQN line low, so the RDYN line will go low later
  */
  if ((false == m_aci_q_is_empty(&aci_tx_q)) &&
    (false == m_aci_q_is_full(&aci_rx_q)))
  {
    digitalWrite(a_pins_local_ptr->reqn_pin, 0);
  }
}

bool hal_aci_tl_event_peek(hal_aci_data_t *p_aci_data)
{
  if (!a_pins_local_ptr->interface_is_interrupt)
  {
    m_aci_device_query ();
  }
  
  if (m_aci_q_peek(&aci_rx_q, p_aci_data))
  {
    if (aci_debug_print)
    {
      Serial.print(" E");
      m_print_aci_data(p_aci_data);
    }

    return true;
  }
 
  return false;
}

bool hal_aci_tl_event_get(hal_aci_data_t *p_aci_data)
{
  if (!a_pins_local_ptr->interface_is_interrupt)
  {
    m_aci_device_query ();
  }
  
  bool was_full = m_aci_q_is_full(&aci_rx_q);
  
  if (m_aci_q_dequeue(&aci_rx_q, p_aci_data))
  {
    if (aci_debug_print)
    {
      Serial.print(" E");
      m_print_aci_data(p_aci_data);
    }
    
    if (was_full && a_pins_local_ptr->interface_is_interrupt)
	  {
      /* Enable RDY line interrupt again */
      EIMSK |= (0x2); /* Make it more portable as this is ATmega specific */
    }
    return true;
  }

  return false;
}

void hal_aci_tl_init(aci_pins_t *a_pins)
{
  received_data.buffer[0] = 0;
  aci_debug_print         = false;
  
  /* Needs to be called as the first thing for proper intialization*/
  m_aci_pins_set(a_pins);
  
  /*
  The SPI lines used are mapped directly to the hardware SPI
  MISO MOSI and SCK
  Change here if the pins are mapped differently
  
  The SPI library assumes that the hardware pins are used
  */
  SPI.begin();
  SPI.setBitOrder(LSBFIRST);
  SPI.setClockDivider(a_pins->spi_clock_divider);
  SPI.setDataMode(SPI_MODE0);
  
  /* initialize aci cmd queue */
  m_aci_q_init(&aci_tx_q);  
  m_aci_q_init(&aci_rx_q);

  //Configure the IO lines
  pinMode(a_pins->rdyn_pin,		INPUT_PULLUP);
  pinMode(a_pins->reqn_pin,		OUTPUT);

  if (UNUSED != a_pins->active_pin)
  {
	pinMode(a_pins->active_pin,	INPUT);  
  }
  
  /* Pin reset the nRF8001 , required when the nRF8001 setup is being changed */
  hal_aci_pin_reset();
	
    
  /* Set the nRF8001 to a known state as required by the datasheet*/
  digitalWrite(a_pins->miso_pin, 0);
  digitalWrite(a_pins->mosi_pin, 0);
  digitalWrite(a_pins->reqn_pin, 1);
  digitalWrite(a_pins->sck_pin,  0);  
  
  delay(30); //Wait for the nRF8001 to get hold of its lines - the lines float for a few ms after the reset
  
  /* Attach the interrupt to the RDYN line as requested by the caller */
  if (a_pins->interface_is_interrupt)
  {
	attachInterrupt(a_pins->interrupt_number, m_rdy_line_handle, LOW); // We use the LOW level of the RDYN line as the atmega328 can wakeup from sleep only on LOW  
  }
}

bool hal_aci_tl_send(hal_aci_data_t *p_aci_cmd)
{
  const uint8_t length = p_aci_cmd->buffer[0];
  bool ret_val = false;

  if (length > HAL_ACI_MAX_LENGTH)
  {
    return false;
  }
  else
  {
    if (m_aci_q_enqueue(&aci_tx_q, p_aci_cmd))
    {
      ret_val = true;
      /*
      Lower the REQN only when successfully enqueued
      */
      digitalWrite(a_pins_local_ptr->reqn_pin, 0);
    }
  }

  if ((true == aci_debug_print) && (true == ret_val))
  {
    Serial.print("C"); //ACI Command
    m_print_aci_data(p_aci_cmd);
  }
  
  return ret_val;
}



hal_aci_data_t * hal_aci_tl_poll_get(void)
{
  uint8_t byte_cnt;
  uint8_t byte_sent_cnt;
  uint8_t max_bytes;
  hal_aci_data_t data_to_send;

  digitalWrite(a_pins_local_ptr->reqn_pin, 0);
  
  // Receive from queue
  if (m_aci_q_dequeue(&aci_tx_q, &data_to_send) == false)
  {
    /* queue was empty, nothing to send */
    data_to_send.status_byte = 0;
    data_to_send.buffer[0] = 0;
  }
  
  //Change this if your mcu has DMA for the master SPI
  
  // Send length, receive header
  byte_sent_cnt = 0;
  received_data.status_byte = spi_readwrite(data_to_send.buffer[byte_sent_cnt++]);
  // Send first byte, receive length from slave
  received_data.buffer[0] = spi_readwrite(data_to_send.buffer[byte_sent_cnt++]);
  if (0 == data_to_send.buffer[0])
  {
    max_bytes = received_data.buffer[0];
  }
  else
  {
    // Set the maximum to the biggest size. One command byte is already sent
    max_bytes = (received_data.buffer[0] > (data_to_send.buffer[0] - 1)) 
      ? received_data.buffer[0] : (data_to_send.buffer[0] - 1);
  }

  if (max_bytes > HAL_ACI_MAX_LENGTH)
  {
    max_bytes = HAL_ACI_MAX_LENGTH;
  }

  // Transmit/receive the rest of the packet 
  for (byte_cnt = 0; byte_cnt < max_bytes; byte_cnt++)
  {
    received_data.buffer[byte_cnt+1] =  spi_readwrite(data_to_send.buffer[byte_sent_cnt++]);
  }

  digitalWrite(a_pins_local_ptr->reqn_pin, 1);

  //RDYN should follow the REQN line in approx 100ns
  
  sleep_enable();
  if (a_pins_local_ptr->interface_is_interrupt)
  {
	attachInterrupt(a_pins_local_ptr->interrupt_number, m_rdy_line_handle, LOW);	  
  }

  if (false == m_aci_q_is_empty(&aci_tx_q))
  {
    //Lower the REQN line to start a new ACI transaction         
    digitalWrite(a_pins_local_ptr->reqn_pin, 0); 
  }
  
  /* valid Rx available or transmit finished*/
  return (&received_data);
}

static uint8_t spi_readwrite(const uint8_t aci_byte)
{
	return SPI.transfer(aci_byte);
}

void m_aci_q_flush(void)
{
  noInterrupts();
  /* re-initialize aci cmd queue and aci event queue to flush them*/
  m_aci_q_init(&aci_tx_q);
  m_aci_q_init(&aci_rx_q);
  interrupts();
}

void m_aci_pins_set(aci_pins_t *a_pins_ptr)
{
  a_pins_local_ptr = a_pins_ptr;	
}

bool hal_aci_tl_rx_q_empty (void)
{
  return m_aci_q_is_empty(&aci_rx_q);
}

bool hal_aci_tl_rx_q_full (void)
{
  return m_aci_q_is_full(&aci_rx_q);
}

bool hal_aci_tl_tx_q_empty (void)
{
  return m_aci_q_is_empty(&aci_tx_q);
}

bool hal_aci_tl_tx_q_full (void)
{
  return m_aci_q_is_full(&aci_tx_q);
}