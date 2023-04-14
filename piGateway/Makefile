install : Gatewayd
	cp Gatewayd /usr/local/bin
	cp GatewaydService /etc/init.d/Gatewayd
	chmod +x /etc/init.d/Gatewayd
	update-rc.d Gatewayd defaults
	service Gatewayd start
	
	
uninstall:
	service Gatewayd stop
	update-rc.d -f Gatewayd remove
	rm /etc/init.d/Gatewayd
	rm /usr/local/bin/Gatewayd

Gatewayd : Gateway.cpp rfm69.cpp rfm69.h rfm69registers.h networkconfig.h 
	g++ Gateway.cpp rfm69.cpp -o Gatewayd -lwiringPi -lmosquitto -DRASPBERRY -DDAEMON

Gateway : Gateway.cpp rfm69.cpp rfm69.h rfm69registers.h networkconfig.h 
	g++ Gateway.cpp rfm69.cpp -o Gateway -lwiringPi -lmosquitto -DRASPBERRY -DDEBUG

SenderReceiver : SenderReceiver.c rfm69.cpp rfm69.h rfm69registers.h networkconfig.h 
	g++ SenderReceiver.c rfm69.cpp -o SenderReceiver -lwiringPi -DRASPBERRY

