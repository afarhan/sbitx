# UI Notes

The sbitx is hackable in various ways. Writing newer and better user interfaces 
is one way to make it better.This is a short note to explain how it the current 
gui written in gtk works.

## UI 
The user interface for a radio is an interesting challenge. It has to work with knobs and buttons as well as a pointer and keyboard. This gtk user interface works with five hardware buttons and two encoders. We use the GTK library to gain access to a screen where we do all the drawing ourselves and draw up all the controls as well. We do chose this route for two reasons:
1. It cuts down on your time to read and understand the intricacies of GTK. Instead, you glance through this document and 100 lines of code, you can get started.
2. The standard user controls like text boxes, buttons etc don't work very well with special hardware as input.

## Data structures
The basic structure used in build the user interface is the struct field:

```c++
struct field {
    char *cmd;
    int x, y, width, height;
    char label[30];
    int label_width;
    char value[MAX_FIELD_LENGTH];
    char value_type; //NUMBER, SELECTION, TEXT, TOGGLE, BUTTON
    int font_index; //refers to font_style table
    char  selection[1000];
    int min, max, step;
};
```

The field is drawn with coordiantes of x,y,width and height. the text of label[] to the left of the value[] printed to the right. The font used is picked up from a table. The field font_index stores the index that is used to select the font from font_table.

Four types of fields are possible : NUMBER (like frequency display), SELECTION - one of many (like USB/LSB or bands), TEXT (rarely used, for instance to send CW), TOGGLE (which can be turned on or off - like AGC) or a BUTTON that triggers a command (""Start recording")

If the field type is SELECTION, the options available are stored in selection field with forward slash between then like "USB/LSB/CW"

For NUMBER field, the minimum and maximum values are stored in the integers with the step. For instance, the step size for frequency display could be 100 Hz and it can range from 500000 to 21500000.

Whenever a field is changed by the user, the updated string of value is concantenated with the cmd field to generate a command to be sent to the sdr radio. For instance "r1:freq=" could be the cmd field and if the value is changed to 14070000, then a command string is created as "r1:freq=14070000". The cmd strings are also used as unique identifiers for each field.

## Drawing the controls
The entire user interface is drawn by our own code using the graphics primitive of drawing lines, rectangles and text. This makes the user interface easy to port to other GUIs or even if we have just a framebuffer to draw on. The controls also use colors from a table.

All the drawing of fields is done in a single function called draw_field(). It handles the drawing of spectrum and waterfall by calling a different function.

## Editing the controls
The controls can be moved between each other with up and down arrow keys or touching them or through one of the encoders. The other encoder can be used to change the value.

## Ticker
Every millisecond, the ui_tick() function is called to perform functions like monitoring any activity on the encoders, switches, Push-To-Talk or the morse key.