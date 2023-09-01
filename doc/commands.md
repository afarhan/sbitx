# Commands

These are text commands that can be entered from the keyboard:
- `\callsign [callsign]`
	Sets your callsign to the following string.

- `\grid [grid]`
	Sets your grid (six letters) that is used in FT8 and other modes to 
	indicate your approximate locationi.

- `\freq [frequency in Hz or Kilohertz]`
	You can also type just 'f' instead 'freq' 
	if you the type '\f 7050' it will set be the same as '\freq 7050000'

- `\cwdelay [100-2000]`
    How long radio remains in transmit in CW before timing out to rx in ms. ex: "\cwdelay 500"

- `\cwinput [key\keyer\kbd]`
	Chooses the CW input method between straight key(key), Iambic keyer(keyer)
	and the keyboard (kbd).

- `\mode [USB\LSB\etc..]`
	Chooses from the modes available in the mode selection control.
	You can use 'm' instead of 'mode'

- `\t` 
	Puts the radio into transmit. You can also use Ctrl-T
	
- `\r` 
	Puts the radio into receive. You can also use Ctrl-R

- `\topen [server]:[port]`
	Opens a telnet session with an RBN or a DX cluster telnet server.
	It works with ip address as well as domain names
	Ex: \topen dxc.g3lrs.org.ul:7300

- `\tclose` 
	Closes the existing telnet session

- `\w [telnet command string]`
	Writes the remaining text (skipping the space after '\w') to the
	currently opened telnet server

- `\txpitch [in Hz]`
	Sets the tone of transmit tone of the CW. 
	Ex: \txpitch 700
