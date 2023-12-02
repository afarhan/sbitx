# Coding Convention

This is a short note on the coding conventions followed with the source code of sbitx.

1. The source code files should be formatted so that they don't go past the right edge of the 
screen on the 800x480 display of the Raspberry Pi.

2. For function and variable names, we use all small case with underscore (_) to seperate out 
the words (like do_cmd or tx_process). 

3. Use static variables that limit the variables' visibility to the particular files and functions
instead of global variables.

4. Practice simpler code over tricky, efficient code. Efficiency should come from eliminating
complex code rather than writing it. Above all, make the code readable and understandable. The
best documentation is cleanly written code.

5. The block braces start on the same line rather than the next. This is to accomodate more
text on the smaller displays used on sbitx (800x480).

6. For any new module, develop it as an independent C program with its own main function that can
be used to exercise it and test it out. When adding to the rest of the code, comment out the
main().

7. Use printf to dump debug information and comment it out when adding to the production.

