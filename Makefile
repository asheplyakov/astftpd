
CC ?= gcc
CFLAGS ?= -std=c99 -Wall -g -O2 -pipe
LDFLAGS ?=
DEPDIR ?= .d

SRC := astftpd.c
OBJ := $(SRC:%.c=%.o)

all: astftpd

astftpd: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(OBJ): %.o: %.c $(DEPDIR)/%.d
	mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td -o $@ -c $<
	mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d


clean:
	-rm -f $(OBJ)
	-rm -f $(DEPDIR)/*.Td $(DEPDIR)/*.d

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d
-include $(patsubst %,$(DEPDIR)/%.d,$(basename $(SRC)))
