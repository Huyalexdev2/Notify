## just a software writed by me
OS support: Windows

Compile:
```
g++ -O2 main.cpp -o Notify -lgdi32 -lgdiplus -luser32 -mwindows
```

if you wanna turn off
use:
```
taskkill /IM Notify.exe /F
```

all write in C++ and have MIT license

Note: no install, just run the program when you want(no data collector, i opened the source)