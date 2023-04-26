/***************************************************************************//**
 *   @file   iio_example.c
 *   @brief  Implementation of IIO example for AD74413R project.
 *   @author Ciprian Regus (ciprian.regus@analog.com)
********************************************************************************
 * Copyright 2022(c) Analog Devices, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  - Neither the name of Analog Devices, Inc. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *  - The use of this software may or may not infringe the patent rights
 *    of one or more patent holders.  This license does not release you
 *    from the requirement that you obtain separate licenses from these
 *    patent holders to use this software.
 *  - Use of the software either in source or binary form, must be run
 *    on or directly connected to an Analog Devices Inc. component.
 *
 * THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, NON-INFRINGEMENT,
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ANALOG DEVICES BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, INTELLECTUAL PROPERTY RIGHTS, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "iio_example.h"
#include "iio_ad74413r.h"
#include "iio_max14906.h"
#include "common_data.h"
#include "no_os_util.h"

#include "iio.h"
#include "iio_types.h"
#include "iiod.h"

#include "no_os_timer.h"
#include "max_eth.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/
#define DATA_BUFFER_SIZE 4000
#define IIO_IGNORE_BUFF_OVERRUN_ERR

/******************************************************************************/
/************************ Variable Declarations ******************************/
/******************************************************************************/

uint8_t iio_data_buffer[DATA_BUFFER_SIZE * sizeof(uint32_t) * 8];
uint8_t iio_data_buffer2[DATA_BUFFER_SIZE * sizeof(uint32_t) * 8];

/******************************************************************************/
/************************ Functions Definitions *******************************/
/******************************************************************************/
/***************************************************************************//**
 * @brief IIO example main execution.
 *
 * @return ret - Result of the example execution. If working correctly, will
 *               execute continuously function iio_app_run and will not return.
*******************************************************************************/
int iio_example_main()
{
	struct tcp_pcb *pcb;
	uint32_t client_id;

	int ret;
	uint32_t reg_val;
	struct iio_desc *iio_desc;
	struct adin1110_desc *adin1110;
	struct iio_hw_trig *ad74413r_trig_desc;
	struct ad74413r_iio_desc *ad74413r_iio_desc;
	struct no_os_irq_ctrl_desc *ad74413r_irq_desc;
	struct ad74413r_iio_desc_init_param ad74413r_iio_ip;
	struct iio_app_desc *app;
	struct iio_data_buffer buff = {
		.buff = (void *)iio_data_buffer,
		.size = DATA_BUFFER_SIZE * sizeof(uint32_t) * 8,
	};
	struct iio_data_buffer buff2 = {
		.buff = (void *)iio_data_buffer2,
		.size = DATA_BUFFER_SIZE * sizeof(uint32_t) * 8,
	};
	struct ad74413r_init_param ad74413r_ip = {
		.chip_id = AD74412R,
		.comm_param = ad74413r_spi_ip
	};
	struct iio_app_init_param app_init_param = { 0 };

	struct max_gpio_init_param max14906_gpio_param = {
		.vssel = 1
	};
	struct no_os_gpio_init_param max14906_gpio_ip = {
		.port = 0,
		.number = 16,
		.platform_ops = &max_gpio_ops,
		.pull = NO_OS_PULL_UP,
		.extra = &max14906_gpio_param
	};
	struct max14906_init_param max14906_ip = {
		.chip_address = 0,
		.comm_param = &max14906_spi_ip,
	};

	struct no_os_timer_init_param eth_tick_param = {
		.id = 0,
		.freq_hz = 64000,
		.ticks_count = 200,
		.platform_ops = &max_timer_ops,
		.extra = NULL,
	};

	struct netif *netif_desc;
	struct max_eth_desc *eth_desc;
	struct max_eth_param eth_param = {
		.name = "e7",
		.adin1110_ip = adin1110_ip,
	};

	struct no_os_irq_ctrl_desc *ad74413r_nvic;
	struct no_os_irq_init_param ad74413r_nvic_ip = {
		.irq_ctrl_id = 0,
		.platform_ops = &max_irq_ops,
		.extra = NULL,
	};

	struct max14906_iio_desc *max14906_iio_desc;
	struct max14906_iio_desc_init_param max14906_iio_ip;

	ret = max_eth_init(&netif_desc, &eth_param);
	if (ret)
		return ret;

	maxim_net.net = netif_desc->state;

	max14906_iio_ip.max14906_init_param = &max14906_ip;
	ad74413r_iio_ip.ad74413r_init_param = &ad74413r_ip;

	ret = no_os_irq_ctrl_init(&ad74413r_nvic, &ad74413r_nvic_ip);
	if (ret)
		return ret;
	
	ret = no_os_irq_enable(ad74413r_nvic, GPIO1_IRQn);
	if (ret)
		return ret;

	/* Initialize interrupt controller */
	ret = no_os_irq_ctrl_init(&ad74413r_irq_desc, &ad74413r_gpio_irq_ip);
	if (ret)
		return ret;

	ret = no_os_irq_set_priority(ad74413r_irq_desc, ad74413r_gpio_irq_ip.irq_ctrl_id, 1);
	if (ret)
		return ret;

	// ret = no_os_irq_enable(ad74413r_irq_desc, ad74413r_irq_desc->irq_ctrl_id);
	// if (ret)
	// 	return ret;

	ad74413r_gpio_trig_ip.irq_ctrl = ad74413r_irq_desc;

	/* Initialize hardware trigger */
	ad74413r_gpio_trig_ip.iio_desc = &iio_desc,
	ret = iio_hw_trig_init(&ad74413r_trig_desc, &ad74413r_gpio_trig_ip);
	if (ret)
		return ret;

	struct iio_trigger_init trigs[] = {
		IIO_APP_TRIGGER(AD74413R_GPIO_TRIG_NAME, ad74413r_trig_desc,
				&ad74413r_iio_trig_desc)
	};

	struct iio_app_device iio_devices[] = {
		{
			.name = "ad74413r",
			.dev = ad74413r_iio_desc,
			.dev_descriptor = ad74413r_iio_desc->iio_dev,
			.read_buff = &buff,
		},
		{
			.name = "max14906",
			.dev = max14906_iio_desc,
			.dev_descriptor = max14906_iio_desc->iio_dev,
			.read_buff = &buff2,
		}
	};

	app_init_param.devices = iio_devices;
	app_init_param.nb_devices = NO_OS_ARRAY_SIZE(iio_devices);
	app_init_param.uart_init_params = adin1110_uart_ip;

	ret = iio_app_init(&app, app_init_param);
	if (ret)
		return ret;

	return iio_app_run(app);
	while (1) {
		/* Probe the iio drivers in config mode */
		ret = ad74413r_iio_init(&ad74413r_iio_desc, &ad74413r_iio_ip, true);
		if (ret)
			return ret;

		ret = max14906_iio_init(&max14906_iio_desc, &max14906_iio_ip, true);
		if (ret)
			return ret;

		iio_devices[0].dev = ad74413r_iio_desc;
		iio_devices[0].dev_descriptor = ad74413r_iio_desc->iio_dev;

		iio_devices[1].dev = max14906_iio_desc;
		iio_devices[1].dev_descriptor = max14906_iio_desc->iio_dev;

		ret = iio_app_run(iio_devices, NO_OS_ARRAY_SIZE(iio_devices));
		if (ret)
			return ret;

		/* Probe the drivers in the run mode */
		ret = max14906_iio_init(&max14906_iio_desc, &max14906_iio_ip, false);
		if (ret)
			return ret;

		ret = ad74413r_iio_init(&ad74413r_iio_desc, &ad74413r_iio_ip, false);
		if (ret)
			return ret;

		iio_devices[0].dev = ad74413r_iio_desc;
		iio_devices[0].dev_descriptor = ad74413r_iio_desc->iio_dev;

		iio_devices[1].dev = max14906_iio_desc;
		iio_devices[1].dev_descriptor = max14906_iio_desc->iio_dev;

		iio_app_run_with_trigs(iio_devices, NO_OS_ARRAY_SIZE(iio_devices),
				       trigs, NO_OS_ARRAY_SIZE(trigs), ad74413r_irq_desc, &iio_desc);
	}
}
