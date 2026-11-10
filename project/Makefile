CC = gcc
CFLAGS = -Wall -Wextra -O2 -Icommon
LDLIBS = -lm
BIN = bin

all: $(BIN)/garden_sim $(BIN)/controller

$(BIN)/garden_sim: device/garden_sim.c device/device.c common/net.c | $(BIN)
	$(CC) $(CFLAGS) -Idevice -o $@ $^ $(LDLIBS)

$(BIN)/controller: controller/controller.c controller/garden_store.c common/net.c | $(BIN)
	$(CC) $(CFLAGS) -Icontroller -o $@ $^

$(BIN):
	mkdir -p $(BIN)

clean:
	rm -rf $(BIN)

# Xoa du lieu chay thu (trang thai thiet bi, log, du lieu app)
reset-data:
	rm -rf state logs data

.PHONY: all clean reset-data
