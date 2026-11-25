# Build overlay only; product Makefile and normal tests remain unchanged.
.PHONY: composition-c02-probe
IDENTITY_SOURCE ?= journal/composition001/c02/identity_probe.c
composition-c02-probe: $(CORE_OBJS) $(LLM_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) "$(IDENTITY_SOURCE)" $(CORE_OBJS) $(LLM_OBJS) -o "$(C02_PROBE)" $(MARIADB_LIBS) -lpthread
	"$(C02_PROBE)"
