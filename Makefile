ifeq ($(UPLOAD_PORT),)
	UPLOAD_PORT := $(shell getent hosts ESP_0016E1 | cut -d' ' -f1)
endif

all:
	platformio run

upload:
	platformio run --target upload --upload-port $(UPLOAD_PORT)

clean:
	platformio run --target clean
