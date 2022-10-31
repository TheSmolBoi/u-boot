#include <common.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <asm/io.h>
#include <spi.h>

#define DEBUG

#define MIN(a,b) ((a <= b) ? a : b)

// Bit 30 in Control: SW Reset
#define OPENTITAN_QSPI_CONTROL_SW_RESET (1 << 30)

#define OPENTITAN_QSPI_TXFIFO_DEPTH 256
#define OPENTITAN_QSPI_RXFIFO_DEPTH 256
#define OPENTITAN_QSPI_FIFO_DEPTH MIN(OPENTITAN_QSPI_TXFIFO_DEPTH, OPENTITAN_QSPI_RXFIFO_DEPTH)

#define OPENTITAN_QSPI_CS_USED 0
#define OPENTITAN_QSPI_CS_UNUSED 1

/* opentitan qspi register set */
enum opentitan_qspi_regs {
	REG_INTR_STATE,		/* Interrupt State Register */
	REG_INTR_ENABLE,	/* Interrupt Enable Register */
	REG_INTR_TEST,		/* Interrupt Test Register */
	REG_ALERT_TEST,		/* Alert Test Register */
	REG_CONTROL,		/* Control Register */
	REG_STATUS,			/* Status Register */
	REG_CONFIGOPTS_0,	/* Configuration Options Register 1 */
	REG_CONFIGOPTS_1,   /* Configuration Options Register 2 */
	REG_CSID,			/* Chip-Select ID */
	REG_COMMAND,		/* Command Register */
	REG_DATA,			/* SPI Data Window*/
	REG_ERROR_ENABLE,	/* Controls which classes of error raise an interrupt */
	REG_ERROR_STATUS,	/* Indicates that any errors have occured */
	REG_EVENT_ENABLE,	/* Controls which classes of SPI events raise an interrupt */
};

/* opentitan qspi priv */
struct opentitan_qspi_priv {
	volatile unsigned int *regs;
  	unsigned int clk_freq; 		/* Peripheral clock frequency */
	unsigned int max_freq; 		/* Max supported SPI frequency */
	unsigned int cs_state; 		/* 0 = CS currently not asserted, 1 = CS currently asserted */
	unsigned char byte_order; 	/* 1 = LSB shifted in/out first, 0 = MSB shifted in/out first */
};

/* opentitan qspi plat */
struct opentitan_qspi_plat {
	volatile unsigned int *regs;
  	unsigned int clk_freq; /* Peripheral clock frequency */
	unsigned int max_freq; /* Max supported SPI frequency */
};

static int opentitan_qspi_of_to_plat(struct udevice *dev)
{
	struct opentitan_qspi_plat *plat = dev_get_plat(dev);
	int ret = 0;
	plat->clk_freq = -1;
	plat->max_freq = -1;
  
	if (!plat)
		return -ENOMEM;

	plat->regs = dev_read_addr_ptr(dev);
	if (!plat->regs)
		return -EINVAL;

    // Try to read the peripheral clock frequency
    ret = dev_read_u32(dev, "clock-frequency", &plat->clk_freq);
    if (ret){
        dev_err(dev, "Unable to find clock-frequency for peripheral!\n");
	    dev_err(dev, "ret: %d, clk_freq = 0x%x\n", ret, plat->clk_freq);
	    return ret;
    }
  
	// Try to read the max frequency, otherwise just use 500 kHz
	plat->max_freq = dev_read_u32_default(dev, "max-frequency", 500000);

	dev_info(dev, "clock-frequency = %d Hz\n", plat->clk_freq);
	dev_info(dev, "max-frequency = %d Hz\n", plat->max_freq);
	dev_info(dev, "regs @ 0x%lx\n", (unsigned long int) &plat->regs);	

	return 0;
}

static int opentitan_qspi_probe(struct udevice *dev)
{
	struct opentitan_qspi_plat *plat = dev_get_plat(dev);
	struct opentitan_qspi_priv *priv = dev_get_priv(dev);
	u32 status = 0, loop_count = 0;

	priv->regs     = plat->regs;
	priv->clk_freq = plat->clk_freq;
    priv->max_freq = plat->max_freq;

	// Disable all interrupts
	writel(0, priv->regs + REG_INTR_ENABLE);
	writel(0, priv->regs + REG_EVENT_ENABLE);

	// Assert SW reset of the SPI Host
	writel(OPENTITAN_QSPI_CONTROL_SW_RESET, priv->regs + REG_CONTROL);

	// Wait until the FIFOs are drained
	do {
		status = (int) readl(priv->regs + REG_STATUS);
		loop_count++;

		if(loop_count >= 1000000){
			writel(0, priv->regs + REG_CONTROL);
			return -EINVAL;
		}
	} while ((status >> 30) & 1 || (status << 16));

	// Deassert SW reset and assert enable signal => Start SPI Host
	writel((1 << 31), priv->regs + REG_CONTROL);

	// Configure the CS
	// De-select the connected peripheral by default
	writel(OPENTITAN_QSPI_CS_UNUSED, priv->regs + REG_CSID);

	// Read the byte order
	status = readl(priv->regs + REG_STATUS);
	priv->byte_order = (status >> 22) & 0x1;

	return 0;
}

static int opentitan_qspi_remove(struct udevice *dev)
{
	return 0;
}


static int opentitan_qspi_issue_dummy(struct udevice *dev, unsigned int bitlen, unsigned long flags)
{
	struct opentitan_qspi_priv *priv = dev_get_priv(dev);
	unsigned char csaat 	= !(flags & SPI_XFER_END) && (priv->cs_state || flags & SPI_XFER_BEGIN);

	if(flags & SPI_XFER_BEGIN){
		priv->cs_state = 1;
		writel(OPENTITAN_QSPI_CS_USED, priv->regs + REG_CSID);
	}

	// Just setting the CS
	if(bitlen == 0){
		if(flags & SPI_XFER_END){
			priv->cs_state = 0;
			writel(OPENTITAN_QSPI_CS_UNUSED, priv->regs + REG_CSID);
			opentitan_qspi_issue_dummy(dev, 8, 0);
		}
		return 0;
	}

	unsigned int status = 0;
	unsigned int command = ((bitlen & 0x1FF) - 1) | ((csaat & 0x1) << 9);
	writel(command, priv->regs + REG_COMMAND);

	do {
		status = readl(priv->regs + REG_STATUS);
	} while((status >> 30) & 0x1);

	if(flags & SPI_XFER_END){
		priv->cs_state = 0;
		writel(OPENTITAN_QSPI_CS_UNUSED, priv->regs + REG_CSID);
	}

	return 0;
}

// Expects the FIFOs to be empty and returns once the FIFOs are empty again
static int opentitan_qspi_xfer_single(struct udevice *child, unsigned int bitlen,
							   const void *dout, void *din, unsigned long flags)
{
	struct udevice *dev = child->parent;

	if(!dout && !din)
		return opentitan_qspi_issue_dummy(dev, bitlen, flags);

	if(bitlen % 8 != 0){
		dev_err(dev, "Transfers must be multiples of 8 bit long\n");
		return -EINVAL;
	}
	
	struct opentitan_qspi_priv *priv = dev_get_priv(dev);
	unsigned int num_bytes 	= bitlen/8;
	unsigned char csaat 	= !(flags & SPI_XFER_END) && (priv->cs_state || flags & SPI_XFER_BEGIN);
	unsigned char dir   	= (din != NULL) | ((dout != NULL) << 1);

	if(flags & SPI_XFER_BEGIN){
		priv->cs_state = 1;
		writel(OPENTITAN_QSPI_CS_USED, priv->regs + REG_CSID);
	}

	// Just setting the CS
	if(bitlen == 0){
		if(flags & SPI_XFER_END){
			priv->cs_state = 0;
			writel(OPENTITAN_QSPI_CS_UNUSED, priv->regs + REG_CSID);
			opentitan_qspi_issue_dummy(dev, 8, 0);
		}
		return 0;
	}

	unsigned int command = 0;
	unsigned int i = 0;
		
	if(dir >> 1){
		// Take care of the word aligned part
		for(; i < num_bytes/4; i++){
			unsigned char tmp[4];

			if(!priv->byte_order){
				tmp[3] = ((unsigned char *) dout)[4*i];
				tmp[2] = ((unsigned char *) dout)[4*i + 1];
				tmp[1] = ((unsigned char *) dout)[4*i + 2];
				tmp[0] = ((unsigned char *) dout)[4*i + 3];
			} else {
				// Read from dout according to its alignment
                // 4 byte
                if(!((long int) dout & 0x3L)){
                    *((unsigned int *) tmp) = *((unsigned int *) (dout+4*i));
 
                // 2 byte
                } else if (!((long int) dout & 0x1L)){
                    *((unsigned short *) tmp)     = *((unsigned short *) (dout+4*i));
                    *((unsigned short *) (tmp+2)) = *((unsigned short *) (dout+4*i+2));
 
                // 1 byte
                } else {
                    tmp[0] = ((unsigned char *) dout)[4*i];
                    tmp[1] = ((unsigned char *) dout)[4*i + 1];
                    tmp[2] = ((unsigned char *) dout)[4*i + 2];
                    tmp[3] = ((unsigned char *) dout)[4*i + 3];
                }
			}

			writel(*((unsigned int *) tmp), priv->regs + REG_DATA);
		}

		// Less than a full word left
		if(i*4 < num_bytes){
			unsigned char tmp[4];

			if(!priv->byte_order){
				// We are in here so at least one byte remains
				tmp[3] = ((unsigned char *) dout)[i*4];
				tmp[2] = ((num_bytes - i*4) >= 2) ? ((unsigned char *) dout)[i*4 + 1] : 0;
				tmp[1] = ((num_bytes - i*4) == 3) ? ((unsigned char *) dout)[i*4 + 2] : 0;
			
				// Cannot need filling as it would have been taken care of by the loop then
				tmp[0] = 0;
			} else {
				// We are in here so at least one byte remains
				tmp[0] = ((unsigned char *) dout)[i*4];
				tmp[1] = ((num_bytes - i*4) >= 2) ? ((unsigned char *) dout)[i*4 + 1] : 0;
				tmp[2] = ((num_bytes - i*4) == 3) ? ((unsigned char *) dout)[i*4 + 2] : 0;
			
				// Cannot need filling as it would have been taken care of by the loop then
				tmp[3] = 0;
			}
			
			writel(*((unsigned int *) tmp), priv->regs + REG_DATA);
		}
	}

	// Set the correct transfer mode
	command = ((num_bytes & 0x1FF) - 1) | ((csaat & 0x1) << 9) | (dir << 12);

	// Start transaction by writing to the command register
	writel(command, priv->regs + REG_COMMAND);

	// Wait for the FIFOs to be empty (full) if we had an actual data transfer
	if(priv->cs_state && dir > 0){
		unsigned int status = 0;

		// RX only or RX/TX
		if(dir == 1 || dir == 3) {
			unsigned int bytes_rcvd = 0;
			do {
				status = readl(priv->regs + REG_STATUS);

				if(((status >> 8) & 0xFF) > 0){
					if(bytes_rcvd < num_bytes){
						unsigned char *dst = (unsigned char *) din;
						unsigned int word = readl(priv->regs + REG_DATA);

						if((num_bytes - bytes_rcvd) >= 4){
							if(!priv->byte_order){
								dst[3] =  word        & 0xFF;
								dst[2] = (word >>  8) & 0xFF;
								dst[1] = (word >> 16) & 0xFF;
								dst[0] = (word >> 24) & 0xFF;
							} else {
								// Store received data into din accordignment
                                // 4 byte
                                if(!((long int) din & 0x3L)){
                                    *((unsigned int *) din) = word;
 
                                // 2 byte
                                } else if (!((long int) din & 0x1L)) {
                                    *((unsigned short *) din)   =  word        & 0xFFFF;
                                    *((unsigned short *) din+2) = (word >> 16) & 0xFFFF;
 
                                // 1 byte
                                } else {
                                    dst[0] =  word        & 0xFF;
                                    dst[1] = (word >>  8) & 0xFF;
                                    dst[2] = (word >> 16) & 0xFF;
                                    dst[3] = (word >> 24) & 0xFF;
                                }
							}

							din += 4;
							bytes_rcvd += 4;
						} else {
							if(!priv->byte_order){
								// We are in here so at least one byte remains
								dst[0] = (word >> 24) & 0xFF;
								bytes_rcvd++;

								if((num_bytes - bytes_rcvd) >= 1){
									dst[1] = (word >> 16) & 0xFF;
									bytes_rcvd++;
								}

								if((num_bytes - bytes_rcvd) == 1){
									dst[2] = (word >> 8) & 0xFF;
									bytes_rcvd++;
								}

							} else {
								// We are in here so at least one byte remains
								dst[0] = word & 0xFF;
                                bytes_rcvd++;

								if((num_bytes - bytes_rcvd) >= 1){
									dst[1] = (word >> 8) & 0xFF;
									bytes_rcvd++;
								}

								if((num_bytes - bytes_rcvd) == 1){
									dst[2] = (word >> 16) & 0xFF;
									bytes_rcvd++;
								}
							}
						}

					// Somehow we have too much data??
					} else {
						(void) readl(priv->regs + REG_DATA);
						dev_err(dev, "Device returned more data than we requested\n");
					}
				}
			} while((((status >> 8) & 0xFF) > 0) || (status >> 30) & 0x1);

			// TODO: take care of non 32-bit read transactions...
			if(bytes_rcvd < num_bytes){
				unsigned int word = readl(priv->regs + REG_DATA);
				unsigned char *dst = (unsigned char *) (din + bytes_rcvd);
				if(!priv->byte_order){
					// We are in here so at least one byte remains
					dst[0] = (word >> 24) & 0xFF;
					bytes_rcvd++;

					if((num_bytes - bytes_rcvd) >= 1){
						dst[1] = (word >> 16) & 0xFF;
						bytes_rcvd++;
					}

					if((num_bytes - bytes_rcvd) == 1){
						dst[2] = (word >> 8) & 0xFF;
						bytes_rcvd++;
					}

				} else {
					// We are in here so at least one byte remains
					dst[0] = word & 0xFF;
                    bytes_rcvd++;

					if((num_bytes - bytes_rcvd) >= 1){
						dst[1] = (word >> 8) & 0xFF;
						bytes_rcvd++;
					}

					if((num_bytes - bytes_rcvd) == 1){
						dst[2] = (word >> 16) & 0xFF;
						bytes_rcvd++;
					}
				}
			}

		// TX Only
		} else if(dir == 2) {
			do {
				status = readl(priv->regs + REG_STATUS);
			} while((status & 0xFF) > 0);
 		
		// What mode is this??
		} else {
			dev_err(dev, "This direction is unknown: %d\n", dir);
			return -EINVAL;
		}
	}

	if(flags & SPI_XFER_END){
		priv->cs_state = 0;
		writel(OPENTITAN_QSPI_CS_UNUSED, priv->regs + REG_CSID);
	}

	return 0;
}

static int opentitan_qspi_xfer(struct udevice *dev, unsigned int bitlen,
							   const void *dout, void *din, unsigned long flags)
{
	// Yay a single transaction
	if(bitlen <= OPENTITAN_QSPI_FIFO_DEPTH*8){
		return opentitan_qspi_xfer_single(dev, bitlen, dout, din, flags);

	// Aww multiple transactions
	} else {
		unsigned long first_flags = flags & SPI_XFER_BEGIN;
		unsigned long last_flags  = flags & SPI_XFER_END;
		unsigned int  num_txns    = (bitlen + OPENTITAN_QSPI_FIFO_DEPTH*8 - 1)/(OPENTITAN_QSPI_FIFO_DEPTH*8);
		
		for(unsigned int i = 0; i < num_txns; i++){
			unsigned long flags = (i == 0) 			? first_flags :
								  (i == num_txns-1) ? last_flags  : 0;
			unsigned int ret = 0;
			unsigned int len = ((bitlen - i*OPENTITAN_QSPI_FIFO_DEPTH*8) < OPENTITAN_QSPI_FIFO_DEPTH*8) ?
								(bitlen - i*OPENTITAN_QSPI_FIFO_DEPTH*8) : OPENTITAN_QSPI_FIFO_DEPTH*8;
			void const *out = NULL;
			void *in  = NULL;

			if(dout)
				out = (void *) (dout + i*OPENTITAN_QSPI_FIFO_DEPTH);

			if(din)
				in  = (void *) (din + i*OPENTITAN_QSPI_FIFO_DEPTH);

			ret = opentitan_qspi_xfer_single(dev, len, out, in, flags);
			
			if(ret)
				return ret;
		}

		return 0;
	}
}

static int opentitan_qspi_set_speed(struct udevice *dev, uint speed)
{
	unsigned long int clkdiv = 0;
	u32 configopts = 0;
	struct opentitan_qspi_priv *priv = dev_get_priv(dev);

	if(speed > priv->max_freq){
		dev_info(dev, "Requested frequency is higher than maximum possible frequency!\n");
		dev_info(dev, "Req: %d, Max: %d\n", speed, priv->max_freq);
		speed = priv->max_freq;
	}

	dev_dbg(dev, "Setting SPI frequency to %d Hz\n", speed);

	// SPI_CLK = SYS_CLK/(2*(clkdiv+1))
	// clkdiv = SYS_CLK/(2*SPI_CLK) - 1

	clkdiv = priv->clk_freq + 2*speed - 1L;
	clkdiv = clkdiv/(2*speed) - 1L;

	if(clkdiv != (clkdiv & (~(-1 << 16)))){
		dev_info(dev, "Calculated clock divider overflows the hardware register! Using maximum value\n");
		clkdiv = ~(-1 << 16);
	}

	configopts = (u32) readl(priv->regs + REG_CONFIGOPTS_0 + OPENTITAN_QSPI_CS_USED);
	configopts = (configopts & (-1 << 16)) | (clkdiv & ~(-1 << 16));
	writel(configopts, priv->regs + REG_CONFIGOPTS_0 + OPENTITAN_QSPI_CS_USED);

	// This is dirty... we are wasting a whole chip select just to be able to control the chipselect
	// independently of the rest of the SPI bus
	writel(configopts, priv->regs + REG_CONFIGOPTS_0 + OPENTITAN_QSPI_CS_UNUSED);

	return 0;
}

static int opentitan_qspi_set_mode(struct udevice *dev, uint mode)
{
	struct opentitan_qspi_priv *priv = dev_get_priv(dev);
	unsigned int configopts = 0;

	configopts = (unsigned int) readl(priv->regs + REG_CONFIGOPTS_0 + OPENTITAN_QSPI_CS_USED);
	configopts = (configopts & 0xFFFF) | (0xFFF << 16) | ((mode & 0x3) << 30);
	writel(configopts, priv->regs + REG_CONFIGOPTS_0 + OPENTITAN_QSPI_CS_USED);
	writel(configopts, priv->regs + REG_CONFIGOPTS_0 + OPENTITAN_QSPI_CS_UNUSED);

	return 0;
}

static const struct dm_spi_ops opentitan_qspi_ops = {
	.xfer			= opentitan_qspi_xfer,
	.set_speed		= opentitan_qspi_set_speed,
	.set_mode		= opentitan_qspi_set_mode,
};

static const struct udevice_id opentitan_qspi_ids[] = {
	{ .compatible = "opentitan,spi-host" },
	{ }
};

U_BOOT_DRIVER(opentitan_qspi) = {
	.name		= "opentitan_qspi",
	.id			= UCLASS_SPI,
	.of_match 	= opentitan_qspi_ids,
	.ops		= &opentitan_qspi_ops,
	.of_to_plat	= opentitan_qspi_of_to_plat,
	.plat_auto	= sizeof(struct opentitan_qspi_plat),
	.priv_auto	= sizeof(struct opentitan_qspi_priv),
	.probe		= opentitan_qspi_probe,
	.remove		= opentitan_qspi_remove
};
