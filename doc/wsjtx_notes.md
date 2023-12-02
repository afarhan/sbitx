# WSJT-X Notes

## Before installing
```
sudo apt --fix-broken install
```

Download the 32 bit package from the wsjt-x page (the one with armhf in the name),

```
sudo dpkg -i wsjt...
```

you may have to run if it complains about the locales

```
sudo pkg-reconfigure locales
```

On WSJT-X
File > Settings > Radio, Choose "Hamlib NET", the NetServer is set to "127.0.0.1"
File > Settings > Radio, Set PTT Method to CAT, Mode to USB 
File > Settings > Audio, Input Set to "plughw:CARD=Loopback,DEV=1", "Left"
File > Settings > Audio, Output Set to "plughw:CARD=Loopback1,DEV=0", "Both"

add the line to /etc/rc.local

```
sudo modprobe snd-aloop enable=1,1,1 index=1,2,3 
```
