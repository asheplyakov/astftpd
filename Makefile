
CC ?= gcc
CFLAGS ?= -std=c99 -Wall -g -O2 -pipe -pthread
LDFLAGS ?= -pthread
DEPDIR ?= .d
SHELL := /bin/bash

SRC := astftpd.c
OBJ := $(SRC:%.c=%.o)
asclient_SRC := asclient.c
asclient_OBJ := $(asclient_SRC:%.c=%.o)

all: astftpd asclient

astftpd: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(OBJ): %.o: %.c $(DEPDIR)/%.d
	mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td -o $@ -c $<
	mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d

asclient: $(asclient_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(asclient_OBJ): %.o: %.c $(DEPDIR)/%.d
	mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td -o $@ -c $<
	mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d

clean:
	-rm -f $(OBJ)
	-rm -f $(asclient_OBJ)
	-rm -f $(DEPDIR)/*.Td $(DEPDIR)/*.d

fixcaps: astftpd
	echo -e 'cap_net_bind_service=+ep\ncap_ipc_lock=+ep\n' | sudo setcap -q - $<

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d
-include $(patsubst %,$(DEPDIR)/%.d,$(basename $(SRC)))
-include $(patsubst %,$(DEPDIR)/%.d,$(basename $(asclient_SRC)))
