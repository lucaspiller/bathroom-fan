ifeq ($(UPLOAD_PORT),)
	UPLOAD_PORT := $(shell getent hosts bathroom-fan | cut -d' ' -f1)
endif

all:
	platformio run

upload:
	platformio run --target upload --upload-port $(UPLOAD_PORT)

clean:
	platformio run --target clean
