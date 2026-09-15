#include "kdnet.h"
#include <drivers/serial/ns16550.h>

/* Build the standard serial KD protocol as a private KDNET emergency backend. */
#define KdD0Transition KdSerialD0Transition
#define KdD3Transition KdSerialD3Transition
#define KdSave KdSerialSave
#define KdRestore KdSerialRestore
#define KdDebuggerInitialize0 KdSerialDebuggerInitialize0
#define KdDebuggerInitialize1 KdSerialDebuggerInitialize1
#define KdReceivePacket KdSerialReceivePacket
#define KdSendPacket KdSerialSendPacket
#define KdNmiTransition KdSerialNmiTransition
#define KdpCalculateChecksum KdpSerialCalculateChecksum
#define KdpSendControlPacket KdpSerialSendControlPacket
#define KdpSendBuffer KdpSerialSendBuffer
#define KdpReceiveBuffer KdpSerialReceiveBuffer
#define KdpReceivePacketLeader KdpSerialReceivePacketLeader
#define KdpSendByte KdpSerialSendByte
#define KdpPollByte KdpSerialPollByte
#define KdpReceiveByte KdpSerialReceiveByte
#define KdpPollBreakIn KdpSerialPollBreakIn
#define KdpPortInitialize KdpSerialPortInitialize
#define CurrentPacketId KdSerialCurrentPacketId
#define RemotePacketId KdSerialRemotePacketId
#define KdComPort KdSerialComPort

#include "../kdcom/kdcom.c"
#include "../kdcom/kdserial.c"
#include "../kdcom/kddll.c"

NTSTATUS
NTAPI
KdSerialInitializeNmi(VOID)
{
    PUCHAR Address = UlongToPtr(0x3080);

    KdSerialCurrentPacketId = INITIAL_PACKET_ID | SYNC_PACKET_ID;
    KdSerialRemotePacketId = INITIAL_PACKET_ID;

    /* The C610 KT UART can fail the destructive scratch/loopback presence
     * tests while its AMT SOL session is between connections. Its PCI BAR was
     * already validated during platform bring-up, so initialize it directly. */
    KdSerialComPort.Address = Address;
    KdSerialComPort.BaudRate = 0;
    KdSerialComPort.Flags = 0;
    WRITE_PORT_UCHAR(Address + LINE_CONTROL_REGISTER, 0);
    WRITE_PORT_UCHAR(Address + INTERRUPT_ENABLE_REGISTER, 0);
    WRITE_PORT_UCHAR(Address + MODEM_CONTROL_REGISTER,
                     SERIAL_MCR_DTR | SERIAL_MCR_RTS | SERIAL_MCR_OUT2);
    CpSetBaud(&KdSerialComPort, 115200);
    WRITE_PORT_UCHAR(Address + LINE_CONTROL_REGISTER,
                     SERIAL_8_DATA | SERIAL_1_STOP | SERIAL_NONE_PARITY);
    CpEnableFifo(Address, TRUE);
    (VOID)READ_PORT_UCHAR(Address + RECEIVE_BUFFER_REGISTER);
    KdComPortInUse = Address;
    return STATUS_SUCCESS;
}
