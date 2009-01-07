%.o: $(S)/%.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(REENT_CFLAGS) -c -o $@ $<
