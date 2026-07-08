; NSF full-configuration corpus -- exercises every v1 statement type.
; comment and blank-line handling are checked below.

DEVICE CTCA CTC 0E20            ; point-to-point CTC device
LINK   LNK1 CTC 0 CTCA         ; link riding over CTCA
HOME   10.1.1.2 LNK1           ; our stack address
GATEWAY DEFAULTNET 10.1.1.1 LNK1 1500 0     ; default route
GATEWAY 192.168.0.0 10.1.1.1 LNK1 1500 255.255.255.0
PORT   23  TCP  TELNET         ; telnet reservation
PORT   161 UDP  SNMPD
TCPCONFIG RECVBUFRSIZE 16384 SENDBUFRSIZE 16384
UDPCONFIG NOUDPCHKSUM
NSFPOOL PBUFSMAL 256
NSFPOOL PBUFLARG 64
NSFTRACE IP ON
NSFTRACE TCP OFF
