INCS += $(PLATFORM_DRIVERS)/$(PLATFORM)_spi.h \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_gpio.h \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_irq.h \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_gpio_irq.h \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_uart.h

SRCS += $(PLATFORM_DRIVERS)/$(PLATFORM)_spi.c \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_gpio.c \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_irq.c \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_gpio_irq.c \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_uart.c \
		$(PLATFORM_DRIVERS)/$(PLATFORM)_delay.c\
		$(PLATFORM_DRIVERS)/$(PLATFORM)_axi_io.c
