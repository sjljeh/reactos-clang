#include "kdnet.h"

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
    KdSerialCurrentPacketId = INITIAL_PACKET_ID | SYNC_PACKET_ID;
    KdSerialRemotePacketId = INITIAL_PACKET_ID;
    return KdpSerialPortInitialize(UlongToPtr(0x3080), 115200);
}
