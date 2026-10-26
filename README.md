# custom_memory_allocator
When a C program calls malloc(100), something has to find 100 free bytes, remember they're in use, and take them back when free() is called. Normally the C library does this. In this project we wrote that "something" ourselves (my_malloc and my_free), then measured it against the system's built-in malloc. 
