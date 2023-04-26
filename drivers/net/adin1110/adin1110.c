/***************************************************************************//**
 *   @file   adin1110.c
 *   @brief  Source file of the ADIN1110 driver.
 *   @author Ciprian Regus (ciprian.regus@analog.com)
********************************************************************************
 * Copyright 2023(c) Analog Devices, Inc.
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

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "no_os_spi.h"
#include "adin1110.h"
#include "no_os_util.h"
#include "no_os_delay.h"
#include "no_os_crc8.h"

#define ADIN1110_CRC_POLYNOMIAL	0x7

NO_OS_DECLARE_CRC8_TABLE(_crc_table);

struct _adin1110_priv {
	uint32_t phy_id;
	uint32_t num_ports;
};

static struct _adin1110_priv driver_data[2] = {
	[ADIN1110] = {
		.phy_id= ADIN1110_PHY_ID,
		.num_ports = 1,
	},
	[ADIN2111] = {
		.phy_id = ADIN2111_PHY_ID,
		.num_ports = 2,
	},
};

/**
 * @brief Write a register's value
 * @param desc - the device descriptor
 * @param addr - register's address
 * @param data - register's value
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_reg_write(struct adin1110_desc *desc, uint16_t addr, uint32_t data)
{
	struct no_os_spi_msg xfer = {
		.tx_buff = desc->tx_buff,
		.bytes_number = ADIN1110_WR_FRAME_SIZE,
		.cs_change = 1,
	};

	/** The address is 13 bit wide */
	addr &= ADIN1110_ADDR_MASK;
	addr |= ADIN1110_CD_MASK | ADIN1110_RW_MASK;
	no_os_put_unaligned_be16(addr, desc->tx_buff);
	no_os_put_unaligned_be32(data, &desc->tx_buff[2]);

	if (desc->append_crc) {
		desc->tx_buff[2] = no_os_crc8(_crc_table, data, 4, 0);
		xfer.bytes_number++;
	}

	return no_os_spi_transfer(desc->comm_desc, &xfer, 1);
}

/**
 * @brief Read a register's value
 * @param desc - the device descriptor
 * @param addr - register's address
 * @param data - register's value
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_reg_read(struct adin1110_desc *desc, uint16_t addr, uint32_t *data)
{
	struct no_os_spi_msg xfer = {
		.tx_buff = desc->tx_buff,
		.rx_buff = desc->rx_buff,
		.bytes_number = ADIN1110_RD_FRAME_SIZE,
		.cs_change = 1,
	};
	int ret;

	no_os_put_unaligned_be16(addr, &desc->tx_buff[0]);
	desc->tx_buff[0] |= ADIN1110_SPI_CD;
	desc->tx_buff[2] = 0x0;

	ret = no_os_spi_transfer(desc->comm_desc, &xfer, 1);
	if (ret)
		return ret;

	*data = no_os_get_unaligned_be32(&desc->rx_buff[3]);

	return 0;
}

/**
 * @brief Update a register's value based on a mask
 * @param desc - the device descriptor
 * @param addr - register's address
 * @param mask - the bits that may be modified
 * @param data - register's value
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_reg_update(struct adin1110_desc *desc, uint16_t addr,
			uint32_t mask, uint32_t data)
{
	uint32_t val;
	int ret;

	ret = adin1110_reg_read(desc, addr, &val);
	if (ret)
		return ret;

	val &= ~mask;
	val |= mask & data;

	return adin1110_reg_write(desc, addr, val);
}

/**
 * @brief Read a PHY register using clause 22
 * @param desc - the device descriptor
 * @param phy_id - the phy device's id
 * @param reg - register's address
 * @param data - register's value
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_mdio_read(struct adin1110_desc *desc, uint32_t phy_id,
		       uint32_t reg, uint32_t *data)
{
	uint32_t mdio_val = 0;
	uint32_t mdio_done = 0;
	uint32_t val = 0;
	int ret;

	/* Use clause 22 for the MDIO register addressing */
	val |= no_os_field_prep(ADIN1110_MDIO_ST, 0x1);
	val |= no_os_field_prep(ADIN1110_MDIO_OP, ADIN1110_MDIO_OP_RD);
	val |= no_os_field_prep(ADIN1110_MDIO_PRTAD, phy_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DEVAD, reg);

	ret = adin1110_reg_write(desc, ADIN1110_MDIOACC(0), val);
	if (ret)
		return ret;

	/* The PHY will set the MDIO_TRDONE bit to 1 once a transaction completes */
	while (!mdio_done) {
		ret = adin1110_reg_read(desc, ADIN1110_MDIOACC(0), &mdio_val);
		if (ret)
			return ret;

		mdio_done = no_os_field_get(ADIN1110_MDIO_TRDONE, mdio_val);
	}

	*data = no_os_field_get(ADIN1110_MDIO_DATA, mdio_val);

	return 0;
}

/**
 * @brief Write a PHY register using clause 22
 * @param desc - the device descriptor
 * @param phy_id - the phy device's id
 * @param reg - register's address
 * @param data - register's value
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_mdio_write(struct adin1110_desc *desc, uint32_t phy_id,
			uint32_t reg, uint32_t data)
{
	uint32_t mdio_val = 0;
	uint32_t mdio_done = 0;
	uint32_t val;
	int ret;

	/* Use clause 22 for the MDIO register addressing */
	val = no_os_field_prep(ADIN1110_MDIO_ST, 0x1);
	val |= no_os_field_prep(ADIN1110_MDIO_OP, ADIN1110_MDIO_OP_WR);
	val |= no_os_field_prep(ADIN1110_MDIO_PRTAD, phy_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DEVAD, reg);
	val |= no_os_field_prep(ADIN1110_MDIO_DATA, data);

	ret = adin1110_reg_write(desc, ADIN1110_MDIOACC(0), val);
	if (ret)
		return ret;

	/* The PHY will set the MDIO_TRDONE bit to 1 once a transaction completes */
	while (!mdio_done) {
		ret = adin1110_reg_read(desc, ADIN1110_MDIOACC(0), &mdio_val);
		if (ret)
			return ret;

		mdio_done = no_os_field_get(ADIN1110_MDIO_TRDONE, mdio_val);
	}

	return 0;
}

/**
 * @brief Write a PHY register using clause 45
 * @param desc - the device descriptor
 * @param phy_id - the phy device's MDIO id
 * @param dev_id - the device id of the register
 * @param reg - register's address
 * @param data - register's value
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_mdio_write_c45(struct adin1110_desc *desc, uint32_t phy_id,
			    uint32_t dev_id, uint32_t reg, uint32_t data)
{
	uint32_t mdio_val = 0;
	uint32_t mdio_done = 0;
	uint32_t val;
	int ret;

	val = no_os_field_prep(ADIN1110_MDIO_ST, 0x0);
	val |= no_os_field_prep(ADIN1110_MDIO_OP, ADIN1110_MDIO_OP_ADDR);
	val |= no_os_field_prep(ADIN1110_MDIO_PRTAD, phy_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DEVAD, dev_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DATA, reg);

	ret = adin1110_reg_write(desc, ADIN1110_MDIOACC(0), val);
	if (ret)
		return ret;

	val = no_os_field_prep(ADIN1110_MDIO_ST, 0x0);
	val |= no_os_field_prep(ADIN1110_MDIO_OP, ADIN1110_MDIO_OP_WR);
	val |= no_os_field_prep(ADIN1110_MDIO_PRTAD, phy_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DEVAD, dev_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DATA, data);

	ret = adin1110_reg_write(desc, ADIN1110_MDIOACC(1), val);
	if (ret)
		return ret;

	/* The PHY will set the MDIO_TRDONE bit to 1 once a transaction completes */
	while (!mdio_done) {
		ret = adin1110_reg_read(desc, ADIN1110_MDIOACC(0), &mdio_val);
		if (ret)
			return ret;

		mdio_done = no_os_field_get(ADIN1110_MDIO_TRDONE, mdio_val);
	}

	mdio_done = 0;
	while (!mdio_done) {
		ret = adin1110_reg_read(desc, ADIN1110_MDIOACC(1), &mdio_val);
		if (ret)
			return ret;

		mdio_done = no_os_field_get(ADIN1110_MDIO_TRDONE, mdio_val);
	}

	return 0;
}

/**
 * @brief Read a PHY register using clause 45
 * @param desc - the device descriptor
 * @param phy_id - the phy device's MDIO id
 * @param dev_id - the device id of the register
 * @param reg - register's address
 * @param data - register's value
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_mdio_read_c45(struct adin1110_desc *desc, uint32_t phy_id,
			   uint32_t dev_id, uint32_t reg, uint32_t *data)
{
	uint32_t mdio_val = 0;
	uint32_t mdio_done = 0;
	uint32_t val;
	int ret;

	val = no_os_field_prep(ADIN1110_MDIO_ST, 0x0);
	val |= no_os_field_prep(ADIN1110_MDIO_OP, ADIN1110_MDIO_OP_ADDR);
	val |= no_os_field_prep(ADIN1110_MDIO_PRTAD, phy_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DEVAD, dev_id);
	val |= no_os_field_prep(ADIN1110_MDIO_DATA, reg);

	ret = adin1110_reg_write(desc, ADIN1110_MDIOACC(0), val);
	if (ret)
		return ret;

	while (!mdio_done) {
		ret = adin1110_reg_read(desc, ADIN1110_MDIOACC(0), &mdio_val);
		if (ret)
			return ret;

		mdio_done = no_os_field_get(ADIN1110_MDIO_TRDONE, mdio_val);
	}

	val = no_os_field_prep(ADIN1110_MDIO_ST, 0x0);
	val |= no_os_field_prep(ADIN1110_MDIO_OP, ADIN1110_MDIO_OP_RD);
	val |= no_os_field_prep(ADIN1110_MDIO_DEVAD, dev_id);
	val |= no_os_field_prep(ADIN1110_MDIO_PRTAD, phy_id);

	ret = adin1110_reg_write(desc, ADIN1110_MDIOACC(1), val);
	if (ret)
		return ret;

	mdio_done = 0;
	while (!mdio_done) {
		ret = adin1110_reg_read(desc, ADIN1110_MDIOACC(1), &mdio_val);
		if (ret)
			return ret;

		mdio_done = no_os_field_get(ADIN1110_MDIO_TRDONE, mdio_val);
	}

	*data = no_os_field_get(ADIN1110_MDIO_DATA, mdio_val);

	return 0;
}

/**
 * @brief Set a MAC address destination filter, frames who's DA doesn't match
 * 	  are dropped.
 * @param desc - the device descriptor
 * @param mac_address - the MAC filter to be set
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_mac_addr_set(struct adin1110_desc *desc,
			  uint8_t mac_address[ADIN1110_ETH_ALEN])
{
	uint32_t reg_val;
	int ret;

	reg_val = no_os_get_unaligned_be16(&mac_address[0]);

	/* Forward frames from both ports to the host */
	reg_val |= ADIN1110_MAC_ADDR_APPLY2PORT | ADIN1110_MAC_ADDR_TO_HOST;
	if (desc->chip_type == ADIN2111)
		reg_val |= ADIN2111_MAC_ADDR_APPLY2PORT2;

	ret = adin1110_reg_update(desc, ADIN1110_MAC_ADDR_FILTER_UPR_REG,
				  NO_OS_GENMASK(31, 0), reg_val);
	if (ret)
		return ret;

	reg_val = no_os_get_unaligned_be32(&mac_address[2]);

	return adin1110_reg_write(desc, ADIN1110_MAC_ADDR_FILTER_LWR_REG, reg_val);
}

/**
 * @brief Write a frame to the TX FIFO.
 * @param desc - the device descriptor
 * @param port - the port for the frame to be transmitted on.
 * @param eth_buff - the frame to be transmitted.
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_write_fifo(struct adin1110_desc *desc, uint32_t port,
			struct adin1110_eth_buff *eth_buff)
{
	uint32_t field_offset = ADIN1110_WR_HEADER_LEN;
	uint32_t padding = 0;
	uint32_t frame_len;
	uint32_t tx_space;
	int ret;

	struct no_os_spi_msg xfer = {
		.tx_buff = desc->tx_buff,
		.cs_change = 1,
	};

	if (port >= driver_data[desc->chip_type].num_ports)
		return -EINVAL;

	/*
	 * The minimum frame length is 64 bytes.
	 * The MAC is by default configured to add the frame check sequence, so it's
	 * length shouldn't be added here.
	 */
	if (eth_buff->payload_len + ADIN1110_ETH_HDR_LEN + ADIN1110_FCS_LEN < 64)
		padding = 64 - (eth_buff->payload_len + ADIN1110_ETH_HDR_LEN + ADIN1110_FCS_LEN);

	frame_len = eth_buff->payload_len + padding + ADIN1110_ETH_HDR_LEN + ADIN1110_FRAME_HEADER_LEN;

	ret = adin1110_reg_read(desc, ADIN1110_TX_SPACE_REG, &tx_space);
	if (ret)
		return ret;

	if (frame_len > 2 * (tx_space - 2))
		return -EAGAIN;

	ret = adin1110_reg_write(desc, ADIN1110_TX_FSIZE_REG,
				 frame_len);
	if (ret)
		return ret;

	/** Align the frame length to 4 bytes */
	frame_len = no_os_align(frame_len, 4);

	memset(desc->tx_buff, 0, frame_len + field_offset);
	no_os_put_unaligned_be16(ADIN1110_TX_REG, &desc->tx_buff[0]);
	desc->tx_buff[0] |= ADIN1110_SPI_CD | ADIN1110_SPI_RW;

	if (desc->append_crc) {
		desc->tx_buff[2] = no_os_crc8(_crc_table, desc->tx_buff, 2, 0);
		field_offset++;
	}

	/* Set the port on which to send the frame */
	no_os_put_unaligned_be16(port, &desc->tx_buff[field_offset]);

	xfer.bytes_number = frame_len + field_offset;
	field_offset += ADIN1110_FRAME_HEADER_LEN;

	memcpy(&desc->tx_buff[field_offset], eth_buff->mac_dest, ADIN1110_ETH_ALEN);
	field_offset += ADIN1110_ETH_ALEN;
	memcpy(&desc->tx_buff[field_offset], eth_buff->mac_source, ADIN1110_ETH_ALEN);
	field_offset += ADIN1110_ETH_ALEN;
	no_os_put_unaligned_be16(eth_buff->ethertype, &desc->tx_buff[field_offset]);
	field_offset += ADIN1110_ETHERTYPE_LEN;
	memcpy(&desc->tx_buff[field_offset], eth_buff->payload,
	       eth_buff->payload_len + padding);

	ret = no_os_spi_transfer(desc->comm_desc, &xfer, 1);
	if (ret)
		return ret;

	ret = adin1110_reg_read(desc, 0x8, &tx_space);
	if (tx_space & NO_OS_BIT(0)) {
		/* Flush TX FIFO */
		ret = adin1110_reg_write(desc, 0x36, 0x2);
		ret = adin1110_reg_write(desc, 0x8, 0x1);
		return -EAGAIN;
	}

	return ret;
}

/**
 * @brief Read a frame from the RX FIFO.
 * @param desc - the device descriptor
 * @param port - the port from which the frame shall be received.
 * @param eth_buff - the frame to be received.
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_read_fifo(struct adin1110_desc *desc, uint32_t port,
		       struct adin1110_eth_buff *eth_buff)
{
	uint32_t field_offset = ADIN1110_RD_HEADER_LEN;
	uint32_t payload_length;
	uint32_t fifo_fsize_reg;
	uint32_t frame_size;
	uint32_t fifo_reg;

	struct no_os_spi_msg xfer = {
		.tx_buff = desc->tx_buff,
		.rx_buff = desc->rx_buff,
		.cs_change = 1,
	};
	int ret;

	if (port >= driver_data[desc->chip_type].num_ports)
		return -EINVAL;

	if (!port) {
		fifo_reg = ADIN1110_RX_REG;
		fifo_fsize_reg = ADIN1110_RX_FSIZE_REG;
	} else {
		fifo_reg = ADIN2111_RX_P2_REG;
		fifo_fsize_reg = ADIN2111_RX_P2_FSIZE_REG;
	}

	ret = adin1110_reg_read(desc, fifo_fsize_reg, &frame_size);
	if (ret)
		return ret;

	payload_length = frame_size - ADIN1110_FRAME_HEADER_LEN - ADIN1110_ETH_HDR_LEN;
	if (frame_size < ADIN1110_FRAME_HEADER_LEN + ADIN1110_FEC_LEN)
		return ret;

	memset(desc->tx_buff, 0, 2048);
	memset(desc->rx_buff, 0, 2048);
	no_os_put_unaligned_be16(fifo_reg, &desc->tx_buff[0]);
	desc->tx_buff[0] |= ADIN1110_SPI_CD;
	desc->tx_buff[2] = 0x00;

	if (desc->append_crc) {
		desc->tx_buff[2] = no_os_crc8(_crc_table, desc->tx_buff, 2, 0);
		field_offset++;
	}

	/* Set the port from which to receive the frame */
	no_os_put_unaligned_be16(port, &desc->tx_buff[field_offset]);

	frame_size = (frame_size + 3) & ~0x3;

	/* Can only read multiples of 4 bytes (the last bytes might be 0) */
	xfer.bytes_number = frame_size + field_offset;
	field_offset += ADIN1110_FRAME_HEADER_LEN;

	/** Burst read the whole frame */
	ret = no_os_spi_transfer(desc->comm_desc, &xfer, 1);
	if (ret)
		return ret;

	memcpy(eth_buff->mac_dest, &desc->rx_buff[field_offset], ADIN1110_ETH_ALEN);
	field_offset += ADIN1110_ETH_ALEN;
	memcpy(eth_buff->mac_source, &desc->rx_buff[field_offset], ADIN1110_ETH_ALEN);
	field_offset += ADIN1110_ETH_ALEN;
	// eth_buff->ethertype = no_os_get_unaligned_be16(&desc->rx_buff[field_offset]);
	eth_buff->ethertype = no_os_get_unaligned_le16(&desc->rx_buff[field_offset]);
	field_offset += ADIN1110_ETHERTYPE_LEN;
	memcpy(eth_buff->payload, &desc->rx_buff[field_offset], payload_length);
	eth_buff->payload_len = payload_length;

	return 0;
}

/**
 * @brief Reset the MAC device.
 * @param desc - the device descriptor
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_mac_reset(struct adin1110_desc *desc)
{
	uint32_t val;
	int ret;

	ret = adin1110_reg_write(desc, ADIN1110_SOFT_RST_REG, ADIN1110_SWRESET_KEY1);
	if (ret)
		return ret;

	ret = adin1110_reg_write(desc, ADIN1110_SOFT_RST_REG, ADIN1110_SWRESET_KEY2);
	if (ret)
		return ret;

	ret = adin1110_reg_write(desc, ADIN1110_SOFT_RST_REG, ADIN1110_SWRELEASE_KEY1);
	if (ret)
		return ret;

	ret = adin1110_reg_write(desc, ADIN1110_SOFT_RST_REG, ADIN1110_SWRELEASE_KEY2);
	if (ret)
		return ret;

	ret = adin1110_reg_read(desc, ADIN1110_MAC_RST_STATUS_REG, &val);
	if (ret)
		return ret;

	if (!val)
		return -EBUSY;

	return 0;
}

/**
 * @brief Complete the reset sequence.
 * @param desc - the device descriptor.
 * @return 0 in case of success, negative error code otherwise
 */
static int adin1110_check_reset(struct adin1110_desc *desc)
{
	uint32_t reg_val;
	int ret;

	ret = adin1110_reg_read(desc, ADIN1110_STATUS0_REG, &reg_val);
	if (ret)
		return ret;

	reg_val = no_os_field_get(ADIN1110_RESETC_MASK, reg_val);
	if (!reg_val)
		return -EBUSY;

	return adin1110_reg_update(desc, ADIN1110_CONFIG1_REG,
				   ADIN1110_CONFIG1_SYNC, ADIN1110_CONFIG1_SYNC);
}

/**
 * @brief Reset the PHY device.
 * @param desc - the device descriptor
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_phy_reset(struct adin1110_desc *desc)
{
	uint32_t phy_id;
	uint32_t expected_id;
	int ret;

	/* The timing values for the reset sequence are spcified in the datasheet */
	ret = no_os_gpio_set_value(desc->reset_gpio, NO_OS_GPIO_LOW);
	if (ret)
		return ret;

	no_os_mdelay(10);

	ret = no_os_gpio_set_value(desc->reset_gpio, NO_OS_GPIO_HIGH);
	if (ret)
		return ret;

	no_os_mdelay(90);

	ret = adin1110_reg_read(desc, ADIN1110_PHY_ID_REG, &phy_id);
	if (ret)
		return ret;

	expected_id = driver_data[desc->chip_type].phy_id;

	if (phy_id != expected_id)
		return -EINVAL;

	return 0;
}

/**
 * @brief Reset both the MAC and PHY.
 * @param desc - the device descriptor
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_sw_reset(struct adin1110_desc *desc)
{
	return adin1110_reg_write(desc, ADIN1110_RESET_REG, 0x1);
}

int adin1110_link_state(struct adin1110_desc *desc, uint32_t *state)
{
	int ret;

	ret = adin1110_reg_read(desc, ADIN1110_STATUS1_REG, state);
	if (ret)
		return ret;

	*state = no_os_field_get(ADIN1110_LINK_STATE_MASK, *state);

	return 0;
}

/**
 * @brief Set a port in promiscuous mode. All MAC filters are dropped.
 * @param desc - the device descriptor
 * @param port - the port for which the mode will be applied
 * @param promisc - either enable or disable the promiscuous mode.
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_set_promisc(struct adin1110_desc *desc, uint32_t port,
			 bool promisc)
{
	uint32_t fwd_mask;

	if (port >= driver_data[desc->chip_type].num_ports)
		return -EINVAL;

	if (!port)
		fwd_mask = ADIN1110_FWD_UNK2HOST_MASK;
	else
		fwd_mask = ADIN2111_P2_FWD_UNK2HOST_MASK;

	return adin1110_reg_update(desc, ADIN1110_CONFIG2_REG, fwd_mask,
				   promisc ? fwd_mask : 0);
}

/**
 * @brief Get the PHY out of software reset.
 * @param desc - the device descriptor
 * @return 0 in case of success, negative error code otherwise
 */
static int adin1110_setup_phy(struct adin1110_desc *desc)
{
	uint32_t reg_val;
	uint32_t ports;
	uint32_t sw_pd;
	size_t i;
	int ret;

	ports = driver_data[desc->chip_type].num_ports;

	for (i = 0; i < ports; i++) {
		ret = adin1110_mdio_read(desc, ADIN1110_MDIO_PHY_ID(i),
					 ADIN1110_MI_CONTROL_REG, &reg_val);
		if (ret)
			return ret;

		/* Get the PHY out of software power down to start the autonegotiation process*/
		sw_pd = no_os_field_get(ADIN1110_MI_SFT_PD_MASK, reg_val);
		if (sw_pd) {
			while (reg_val & ADIN1110_MI_SFT_PD_MASK) {
				reg_val &= ~ADIN1110_MI_SFT_PD_MASK;
				ret = adin1110_mdio_write(desc, ADIN1110_MDIO_PHY_ID(i),
							  ADIN1110_MI_CONTROL_REG, reg_val);
				if (ret)
					return ret;
				ret = adin1110_mdio_read(desc, ADIN1110_MDIO_PHY_ID(i),
							 ADIN1110_MI_CONTROL_REG, &reg_val);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

/**
 * @brief Enable CRC, interrupts and a MAC filter for the MAC device.
 * @param desc - the device descriptor
 * @return 0 in case of success, negative error code otherwise
 */
static int adin1110_setup_mac(struct adin1110_desc *desc)
{
	int ret;
	uint32_t reg_val;

	ret = adin1110_reg_update(desc, ADIN1110_CONFIG2_REG, ADIN1110_CRC_APPEND,
				  ADIN1110_CRC_APPEND);
	if (ret)
		return ret;

	ret = adin1110_reg_read(desc, ADIN1110_CONFIG2_REG, &reg_val);
	if (ret)
		return ret;

	reg_val = ADIN1110_TX_RDY_IRQ | ADIN1110_RX_RDY_IRQ | ADIN1110_SPI_ERR_IRQ | NO_OS_BIT(1);
	if (desc->chip_type == ADIN2111)
		reg_val |= ADIN2111_RX_RDY_IRQ;

	ret = adin1110_reg_write(desc, ADIN1110_IMASK1_REG, reg_val);
	if (ret)
		return ret;

	return adin1110_mac_addr_set(desc, desc->mac_address);
}

/**
 * @brief Initialize the device
 * @param desc - the device descriptor to be initialized
 * @param param - the device's parameter
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_init(struct adin1110_desc **desc,
		  struct adin1110_init_param *param)
{
	struct adin1110_desc *descriptor;
	int ret;

	descriptor = calloc(1, sizeof(*descriptor));
	if (!descriptor)
		return -ENOMEM;

	ret = no_os_spi_init(&descriptor->comm_desc, &param->comm_param);
	if (ret)
		goto free_desc;

	no_os_crc8_populate_msb(_crc_table, ADIN1110_CRC_POLYNOMIAL);
	strncpy((char *)descriptor->mac_address, (char *)param->mac_address,
		ADIN1110_MAC_LEN);

	ret = no_os_gpio_get(&descriptor->reset_gpio, &param->reset_param);
	if (ret)
		goto free_spi;

	ret = no_os_gpio_direction_output(descriptor->reset_gpio, NO_OS_GPIO_OUT);
	if (ret)
		goto free_rst_gpio;

	ret = no_os_gpio_get(&descriptor->int_gpio, &param->int_param);
	if (ret)
		goto free_rst_gpio;

	ret = no_os_gpio_direction_input(descriptor->int_gpio);
	if (ret)
		goto free_int_gpio;

	if (!param->mac_address) {
		ret = -EINVAL;
		goto free_int_gpio;
	}

	memcpy(descriptor->mac_address, param->mac_address, ADIN1110_ETH_ALEN);
	descriptor->chip_type = param->chip_type;

	ret = adin1110_sw_reset(descriptor);
	if (ret)
		goto free_int_gpio;

	/* Wait for the MAC and PHY digital interface to intialize */
	no_os_mdelay(90);

	ret = no_os_gpio_set_value(descriptor->reset_gpio, NO_OS_GPIO_HIGH);
	if (ret)
		goto free_int_gpio;

	ret = adin1110_setup_mac(descriptor);
	if (ret)
		goto free_int_gpio;

	ret = adin1110_setup_phy(descriptor);
	if (ret)
		goto free_int_gpio;

	ret = adin1110_check_reset(descriptor);
	if (ret)
		goto free_int_gpio;

	ret = adin1110_reg_write(descriptor, 0x3E, 0x77);
	if (ret)
		return ret;

	*desc = descriptor;

	return 0;

free_int_gpio:
	no_os_gpio_remove(descriptor->int_gpio);
free_rst_gpio:
	no_os_gpio_remove(descriptor->reset_gpio);
free_spi:
	no_os_spi_remove(descriptor->comm_desc);
free_desc:
	free(descriptor);

	return ret;
}

/**
 * @brief Free a device descriptor
 * @param desc - the device descriptor to be removed.
 * @return 0 in case of success, negative error code otherwise
 */
int adin1110_remove(struct adin1110_desc *desc)
{
	int ret;

	if (!desc)
		return -EINVAL;

	ret = no_os_spi_remove(desc->comm_desc);
	if (ret)
		return ret;

	ret = no_os_gpio_remove(desc->reset_gpio);
	if (ret)
		return ret;

	free(desc);

	return 0;
}