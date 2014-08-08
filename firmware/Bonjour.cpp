//  Copyright (c) 2014 Alex Skalozub
//  pieceofsummer@gmail.com
//
//  Bonjour service discovery for Spark Core Wi-Fi chipset.
//  Based on EthernetBonjour for Arduino by Georg Kaindl.
//
//  Original header:
//
//  Copyright (C) 2010 Georg Kaindl
//  http://gkaindl.com
//
//  This file is part of Arduino EthernetBonjour.
//
//  EthernetBonjour is free software: you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public License
//  as published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version.
//
//  EthernetBonjour is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with EthernetBonjour. If not, see
//  <http://www.gnu.org/licenses/>.
//

#define  HAS_SERVICE_REGISTRATION      1  // disabling saves about 1.25 kilobytes
#define  HAS_NAME_BROWSING             1  // disable together with above, additionally saves about 4.3 kilobytes

#include <string.h>
#include <stdlib.h>

#include "Bonjour.h"

#define  MDNS_DEFAULT_NAME       "myspark"
#define  MDNS_TLD                ".local"
#define  DNS_SD_SERVICE          "_services._dns-sd._udp.local"
#define  MDNS_SERVER_PORT        (5353)
#define  MDNS_NQUERY_RESEND_TIME (1000)   // 1 second, name query resend timeout
#define  MDNS_SQUERY_RESEND_TIME (10000)  // 10 seconds, service query resend timeout
#define  MDNS_RESPONSE_TTL       (120)    // two minutes (in seconds)

#define  MDNS_MAX_SERVICES_PER_PACKET  (6)

//#define  _BROKEN_MALLOC_   1
#undef _USE_MALLOC_

static IPAddress mdnsMulticastIPAddr(224, 0, 0, 251);

typedef enum _MDNSPacketType_t {
   MDNSPacketTypeMyIPAnswer,
   MDNSPacketTypeNoIPv6AddrAvailable,
   MDNSPacketTypeServiceRecord,
   MDNSPacketTypeServiceRecordRelease,
   MDNSPacketTypeNameQuery,
   MDNSPacketTypeServiceQuery,
} MDNSPacketType_t;

typedef struct _DNSHeader_t {
   uint16_t    xid;
   uint8_t     recursionDesired:1;
   uint8_t     truncated:1;
   uint8_t     authoritiveAnswer:1;
   uint8_t     opCode:4;
   uint8_t     queryResponse:1;
   uint8_t     responseCode:4;
   uint8_t     checkingDisabled:1;
   uint8_t     authenticatedData:1;
   uint8_t     zReserved:1;
   uint8_t     recursionAvailable:1;
   uint16_t    queryCount;
   uint16_t    answerCount;
   uint16_t    authorityCount;
   uint16_t    additionalCount;
} __attribute__((__packed__)) DNSHeader_t;

typedef enum _DNSOpCode_t {
   DNSOpQuery     = 0,
   DNSOpIQuery    = 1,
   DNSOpStatus    = 2,
   DNSOpNotify    = 4,
   DNSOpUpdate    = 5
} DNSOpCode_t;

// for some reason, I get data corruption issues with normal malloc() on arduino 0017
void* my_malloc(unsigned s)
{
#if defined(_BROKEN_MALLOC_)
   char* b = (char*)malloc(s+2);
   if (b)
      b++;
   
   return (void*)b;
#else
   return malloc(s);
#endif
}

void my_free(void* ptr)
{
#if defined(_BROKEN_MALLOC_)
   char* b = (char*)ptr;
   if (b)
      b--;
   
   free(b);
#else
   free(ptr);
#endif
}

BonjourClass::BonjourClass()
{
   memset(&_mdnsData, 0, sizeof(MDNSDataInternal_t));
   memset(&_serviceRecords, 0, sizeof(_serviceRecords));
   
   _state = MDNSStateIdle;
   _writeOffset = 0;
   
   _bonjourName = NULL;
   _resolveNames[0] = NULL;
   _resolveNames[1] = NULL;
   
   _lastAnnounceMillis = 0;
}

BonjourClass::~BonjourClass()
{
    stop();
}

int BonjourClass::beginPacket(IPAddress ip, uint16_t port)
{
    // send previous packet (if not sent)
    if (_writeOffset > 0)
        (void)endPacket();
    
    _writeOffset = 0;
    return UDP::beginPacket(ip, port);
}

size_t BonjourClass::write(const uint8_t *buffer, size_t len)
{
    size_t empty = sizeof(_writeBuffer) - _writeOffset;
    if (len > empty) len = empty;
    memcpy(_writeBuffer + _writeOffset, buffer, len);
    _writeOffset += len;
    return len;
}

int BonjourClass::endPacket()
{
    int r = UDP::write(_writeBuffer, _writeOffset);
    _writeOffset = 0;
    return r;
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::begin(const char* bonjourName)
{
	// wait for network to be ready
	while (millis() < 5000 && !WiFi.ready()) SPARK_WLAN_Loop();

	int statusCode = 0;
	statusCode = setBonjourName(bonjourName);
	if (statusCode)
	    statusCode = UDP::begin(MDNS_SERVER_PORT);

	return statusCode;
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::begin()
{
    return begin(MDNS_DEFAULT_NAME);
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::_initQuery(uint8_t idx, const char* name, unsigned long timeout)
{
    int statusCode = 0;
    
    if (NULL == _resolveNames[idx] && NULL != ((0 == idx) ? (void*)_nameFoundCallback : (void*)_serviceFoundCallback)) 
    {
        _resolveNames[idx] = (uint8_t*)name;
      
        if (timeout)
            _resolveTimeouts[idx] = millis() + timeout;
        else
            _resolveTimeouts[idx] = 0;
      
        statusCode = (MDNSSuccess == _sendMDNSMessage(NULL, 0, (idx == 0) ? MDNSPacketTypeNameQuery : MDNSPacketTypeServiceQuery, 0));
    } 
    else
        my_free((void*)name);
    
    return statusCode;
}

void BonjourClass::_cancelQuery(uint8_t idx)
{
    if (NULL == _resolveNames[idx]) return;
    my_free(_resolveNames[idx]);
    _resolveNames[idx] = NULL;
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::resolveName(const char* name, unsigned long timeout)
{   
	cancelResolveName();
   
	char* n = (char*)my_malloc(strlen(name) + 7);
	if (NULL == n)
		return 0;
   
	strcpy(n, name);
   	strcat(n, MDNS_TLD);
   	return _initQuery(0, n, timeout);
}

void BonjourClass::setNameResolvedCallback(BonjourNameFoundCallback newCallback)
{
   	_nameFoundCallback = newCallback;
}

void BonjourClass::cancelResolveName()
{
   	_cancelQuery(0);
}

int BonjourClass::isResolvingName()
{
	return (NULL != _resolveNames[0]);
}

void BonjourClass::setServiceFoundCallback(BonjourServiceFoundCallback newCallback)
{
	_serviceFoundCallback = newCallback;
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::startDiscoveringService(const char* serviceName, MDNSServiceProtocol_t proto, unsigned long timeout)
{   
	stopDiscoveringService();
   
	char* n = (char*)my_malloc(strlen(serviceName) + 13);
	if (NULL == n)
		return 0;
   
	strcpy(n, serviceName);   
         
	const uint8_t* srv_type = _postfixForProtocol(proto);
	if (srv_type)
		strcat(n, (const char*)srv_type);
   
	_resolveServiceProto = proto;
	return _initQuery(1, n, timeout);
}

void BonjourClass::stopDiscoveringService()
{
	_cancelQuery(1);
}

int BonjourClass::isDiscoveringService()
{
	return (NULL != _resolveNames[1]);
}

// return value:
// A DNSError_t (DNSSuccess on success, something else otherwise)
// in "int" mode: positive on success, negative on error
MDNSError_t BonjourClass::_sendMDNSMessage(IPAddress *peerAddress, uint32_t xid, int type, int serviceRecord)
{
    MDNSError_t statusCode = MDNSSuccess;
    uint16_t ptr = 0;
#if defined(_USE_MALLOC_)
    DNSHeader_t* dnsHeader = NULL;
#else
    DNSHeader_t dnsHeaderBuf;
    DNSHeader_t* dnsHeader = &dnsHeaderBuf;
#endif
    uint8_t* buf;
   

#if defined(_USE_MALLOC_)
    dnsHeader = (DNSHeader_t*)my_malloc(sizeof(DNSHeader_t));
    if (NULL == dnsHeader) {
        statusCode = MDNSOutOfMemory;
        goto errorReturn;
    }
#endif
      
    memset(dnsHeader, 0, sizeof(DNSHeader_t));
   
    dnsHeader->xid = htons(xid);
    dnsHeader->opCode = DNSOpQuery;
   
    switch (type) 
    {
        case MDNSPacketTypeServiceRecordRelease:
        case MDNSPacketTypeMyIPAnswer:
            dnsHeader->answerCount = htons(1);
            dnsHeader->queryResponse = 1;
            dnsHeader->authoritiveAnswer = 1;
            break;
        case MDNSPacketTypeServiceRecord:
            dnsHeader->answerCount = htons(4);
            dnsHeader->additionalCount = htons(1);
            dnsHeader->queryResponse = 1;
            dnsHeader->authoritiveAnswer = 1;
            break;
        case MDNSPacketTypeNameQuery:
        case MDNSPacketTypeServiceQuery:
            dnsHeader->queryCount = htons(1);
            break;
        case MDNSPacketTypeNoIPv6AddrAvailable:
            dnsHeader->queryCount = htons(1);
            dnsHeader->additionalCount = htons(1);
            dnsHeader->responseCode = 0x03;
            dnsHeader->authoritiveAnswer = 1;
            dnsHeader->queryResponse = 1;
            break;
    }


    beginPacket(mdnsMulticastIPAddr, MDNS_SERVER_PORT);
    write((uint8_t*)dnsHeader, sizeof(DNSHeader_t));

    ptr += sizeof(DNSHeader_t);
    buf = (uint8_t*)dnsHeader;
   
    // construct the answer section
    switch (type) 
    {
        case MDNSPacketTypeMyIPAnswer: 
        {
            _writeMyIPAnswerRecord(&ptr, buf, sizeof(DNSHeader_t));
            break;
        }

#if defined(HAS_SERVICE_REGISTRATION) && HAS_SERVICE_REGISTRATION
      
        case MDNSPacketTypeServiceRecord: 
        {
            // SRV location record
            _writeServiceRecordName(serviceRecord, &ptr, buf, sizeof(DNSHeader_t), 0);
         
            buf[0] = 0x00;
            buf[1] = 0x21;    // SRV record
            buf[2] = 0x80;    // cache flush
            buf[3] = 0x01;    // class IN
         
            // ttl
            *((uint32_t*)&buf[4]) = htonl(MDNS_RESPONSE_TTL);
         
            // data length
            *((uint16_t*)&buf[8]) = htons(8 + strlen((char*)_bonjourName));

            write((uint8_t*)buf, 10);
            ptr += 10;
            // priority and weight
            buf[0] = buf[1] = buf[2] = buf[3] = 0;
         
            // port
            *((uint16_t*)&buf[4]) = htons(_serviceRecords[serviceRecord]->port);
         
            write((uint8_t*)buf, 6);
            ptr += 6;
            // target
            _writeDNSName(_bonjourName, &ptr, buf, sizeof(DNSHeader_t), 1);
         
            // TXT record
            _writeServiceRecordName(serviceRecord, &ptr, buf, sizeof(DNSHeader_t), 0);
         
            buf[0] = 0x00;
            buf[1] = 0x10;    // TXT record
            buf[2] = 0x80;    // cache flush
            buf[3] = 0x01;    // class IN
         
            // ttl
            *((uint32_t*)&buf[4]) = htonl(MDNS_RESPONSE_TTL);

            write((uint8_t*)buf, 8);
            ptr += 8;
         
            // data length && text
            if (NULL == _serviceRecords[serviceRecord]->textContent) {
                buf[0] = 0x00;
                buf[1] = 0x01;
                buf[2] = 0x00;
                
                write((uint8_t*)buf, 3);
                ptr += 3;
            } else {
                int slen = strlen((const char*)_serviceRecords[serviceRecord]->textContent);
                *((uint16_t*)buf) = htons(slen);
                write((uint8_t*)buf, 2);
                ptr += 2;
    
                write((uint8_t*)_serviceRecords[serviceRecord]->textContent, slen);
                ptr += slen;
            }
         
            // PTR record (for the dns-sd service in general)
            _writeDNSName((const uint8_t*)DNS_SD_SERVICE, &ptr, buf, sizeof(DNSHeader_t), 1);
         
            buf[0] = 0x00;
            buf[1] = 0x0c;    // PTR record
            buf[2] = 0x00;    // no cache flush
            buf[3] = 0x01;    // class IN
         
            // ttl
            *((uint32_t*)&buf[4]) = htonl(MDNS_RESPONSE_TTL);
         
            // data length.
            uint16_t dlen = strlen((char*)_serviceRecords[serviceRecord]->servName) + 2;
            *((uint16_t*)&buf[8]) = htons(dlen);
            
            write((uint8_t*)buf, 10);
            ptr += 10;
         
            _writeServiceRecordName(serviceRecord, &ptr, buf, sizeof(DNSHeader_t), 1);
         
            // PTR record (our service)
            _writeServiceRecordPTR(serviceRecord, &ptr, buf, sizeof(DNSHeader_t), MDNS_RESPONSE_TTL);
         
            // finally, our IP address as additional record
            _writeMyIPAnswerRecord(&ptr, buf, sizeof(DNSHeader_t));
            break;
        }
      
        case MDNSPacketTypeServiceRecordRelease: 
        {
            // just send our service PTR with a TTL of zero
            _writeServiceRecordPTR(serviceRecord, &ptr, buf, sizeof(DNSHeader_t), 0);
            break;
        }
      
#endif // defined(HAS_SERVICE_REGISTRATION) && HAS_SERVICE_REGISTRATION

#if defined(HAS_NAME_BROWSING) && HAS_NAME_BROWSING
    
        case MDNSPacketTypeNameQuery:
        case MDNSPacketTypeServiceQuery: 
        {
            // construct a query for the currently set _resolveNames[0]
            _writeDNSName(
               (type == MDNSPacketTypeServiceQuery) ? _resolveNames[1] : _resolveNames[0],
               &ptr, buf, sizeof(DNSHeader_t), 1);

            buf[0] = buf[2] = 0x0;
            buf[1] = (type == MDNSPacketTypeServiceQuery) ? 0x0c : 0x01; 
            buf[3] = 0x1;

            write((uint8_t*)buf, sizeof(DNSHeader_t));
            ptr += sizeof(DNSHeader_t);
         
            _resolveLastSendMillis[(type == MDNSPacketTypeServiceQuery) ? 1 : 0] = millis();
            break;
        }
      
#endif // defined(HAS_NAME_BROWSING) && HAS_NAME_BROWSING
      
        case MDNSPacketTypeNoIPv6AddrAvailable: 
        {
            // since the Spark doesn't have IPv6, we will respond with a Not Found message
            _writeDNSName(_bonjourName, &ptr, buf, sizeof(DNSHeader_t), 1);
         
            buf[0] = buf[2] = 0x0;
            buf[1] = 0x1c; // AAAA record
            buf[3] = 0x01;

            write((uint8_t*)buf, 4);
            ptr += 4;
         
            // send our IPv4 address record as additional record, in case the peer wants it.
            _writeMyIPAnswerRecord(&ptr, buf, sizeof(DNSHeader_t));
            break;
        }
    }

    endPacket();

#if defined(_USE_MALLOC_)
errorReturn:

    if (NULL != dnsHeader)
        my_free(dnsHeader);
#endif
   
	return statusCode;
}

// return value:
// A DNSError_t (DNSSuccess on success, something else otherwise)
// in "int" mode: positive on success, negative on error
MDNSError_t BonjourClass::_processMDNSQuery()
{
    MDNSError_t statusCode = MDNSSuccess;
#if defined(_USE_MALLOC_)
    DNSHeader_t* dnsHeader = NULL;
#else
    DNSHeader_t dnsHeaderBuf;
    DNSHeader_t* dnsHeader = &dnsHeaderBuf;
#endif
    uint8_t* buf;
    uint32_t xid;
    uint16_t udp_len, qCnt, aCnt, aaCnt, addCnt;
    uint8_t recordsAskedFor[NumMDNSServiceRecords+2];
    uint8_t recordsFound[2];
    uint8_t wantsIPv6Addr = 0;
    uint8_t * udpBuffer = NULL;
    uintptr_t ptr;

    memset(recordsAskedFor, 0, sizeof(uint8_t)*(NumMDNSServiceRecords+2));
    memset(recordsFound, 0, sizeof(uint8_t)*2);

    udp_len = parsePacket();
    if (0 == udp_len) {
        statusCode = MDNSTryLater;
        goto errorReturn;
    }

    udpBuffer = (uint8_t*) my_malloc(udp_len);  //allocate memory to hold _remaining UDP packet
    read(udpBuffer, udp_len);
    ptr = (uintptr_t)udpBuffer;

#if defined(_USE_MALLOC_)
    dnsHeader = (DNSHeader_t*)my_malloc(sizeof(DNSHeader_t));
    if (NULL == dnsHeader) {
        statusCode = MDNSOutOfMemory;
        goto errorReturn;
    }
#endif

    buf = (uint8_t*)dnsHeader;
    memcpy((uint8_t*)buf, (uint16_t*)ptr ,sizeof(DNSHeader_t));
   
    xid = ntohs(dnsHeader->xid);
    qCnt = ntohs(dnsHeader->queryCount);
    aCnt = ntohs(dnsHeader->answerCount);
    aaCnt = ntohs(dnsHeader->authorityCount);
    addCnt = ntohs(dnsHeader->additionalCount);

    if (0 == dnsHeader->queryResponse && DNSOpQuery == dnsHeader->opCode && MDNS_SERVER_PORT == remotePort())
    {
        // process an MDNS query
        int offset = sizeof(DNSHeader_t);
        uint8_t* buf = (uint8_t*)dnsHeader;
        int rLen = 0, tLen = 0;

        // read over the query section 
        for (uint16_t i = 0; i < qCnt; i++) 
        {         
            // construct service name data structures for comparison
            const uint8_t* servNames[NumMDNSServiceRecords+2];
            int servLens[NumMDNSServiceRecords+2];
            uint8_t servNamePos[NumMDNSServiceRecords+2];
            uint8_t servMatches[NumMDNSServiceRecords+2];
         
            // first entry is our own MDNS name, the rest are our services
            servNames[0] = (const uint8_t*)_bonjourName;
            servNamePos[0] = 0;
            servLens[0] = strlen((char*)_bonjourName);
            servMatches[0] = 1;
         
            // second entry is our own the general DNS-SD service
            servNames[1] = (const uint8_t*)DNS_SD_SERVICE;
            servNamePos[1] = 0;
            servLens[1] = strlen((char*)DNS_SD_SERVICE);
            servMatches[1] = 1;
                  
            for (uint8_t j = 2; j < NumMDNSServiceRecords + 2; j++)
            {
                if (NULL != _serviceRecords[j-2] && NULL != _serviceRecords[j-2]->servName) {
                    servNames[j] = _serviceRecords[j-2]->servName;
                    servLens[j] = strlen((char*)servNames[j]);
                    servMatches[j] = 1;
                    servNamePos[j] = 0;
                } else {
                    servNames[j] = NULL;
                    servLens[j] = 0;
                    servMatches[j] = 0;
                    servNamePos[j] = 0;
                }
            }
            
            tLen = 0;
            do {
        	    memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset), 1);
                offset += 1;
                
                rLen = buf[0];
                tLen += 1;
            
                if (rLen > 128) { // handle DNS name compression, kinda, sorta
            	    memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,1);
            	    offset += 1;
               
                    for (uint8_t j = 0; j < NumMDNSServiceRecords + 2; j++) {
                        if (servNamePos[j] && servNamePos[j] != buf[0]) {
                            servMatches[j] = 0;
                        }
                    }
               
                    tLen += 1;
                } else if (rLen > 0) {
                    int tr = rLen, ir;
               
                    while (tr > 0) {
                        ir = ((size_t)tr > sizeof(DNSHeader_t)) ? sizeof(DNSHeader_t) : tr;

                        memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,ir);
                        offset += ir;
                        tr -= ir;
                  
                        for (uint8_t j = 0; j < NumMDNSServiceRecords + 2; j++) {
                            if (!recordsAskedFor[j] && servMatches[j])
                                servMatches[j] &= _matchStringPart(&servNames[j], &servLens[j], buf, ir);
                        }
                    }
               
                    tLen += rLen;
                }
            } while (rLen > 0 && rLen <= 128);
            
            // if this matched a name of ours (and there are no characters left), then
            // check whether this is an A record query (for our own name) or a PTR record query
            // (for one of our services).
            // if so, we'll note to send a record
            
            memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset), 4);
            offset += 4;
         
            for (uint8_t j = 0; j < NumMDNSServiceRecords + 2; j++) 
            {
                if (!recordsAskedFor[j] && servNames[j] && servMatches[j] && 0 == servLens[j]) 
                {
                    if (0 == servNamePos[j])
                        servNamePos[j] = offset - 4 - tLen;
               
                    if (buf[0] == 0 && buf[3] == 0x01 && (buf[2] == 0x00 || buf[2] == 0x80)) 
                    {
                        if ((0 == j && 0x01 == buf[1]) || (0 < j && (0x0c == buf[1] || 0x10 == buf[1] || 0x21 == buf[1])))
                            recordsAskedFor[j] = 1;
                        else if (0 == j && 0x1c == buf[1])
                            wantsIPv6Addr = 1;
                    }
                }
            }
        }
    } 
   
#if (defined(HAS_SERVICE_REGISTRATION) && HAS_SERVICE_REGISTRATION) || (defined(HAS_NAME_BROWSING) && HAS_NAME_BROWSING)

    else if (1 == dnsHeader->queryResponse && DNSOpQuery == dnsHeader->opCode && MDNS_SERVER_PORT == remotePort() && 
        (NULL != _resolveNames[0] || NULL != _resolveNames[1]))
    {
        int offset = sizeof(DNSHeader_t);
        uint8_t* buf = (uint8_t*)dnsHeader;
        int rLen = 0, tLen = 0;
         
        uint8_t* ptrNames[MDNS_MAX_SERVICES_PER_PACKET];
        uint16_t ptrOffsets[MDNS_MAX_SERVICES_PER_PACKET];
        uint16_t ptrPorts[MDNS_MAX_SERVICES_PER_PACKET];
        uint8_t ptrIPs[MDNS_MAX_SERVICES_PER_PACKET];
        uint8_t servIPs[MDNS_MAX_SERVICES_PER_PACKET][5];
        uint8_t* servTxt[MDNS_MAX_SERVICES_PER_PACKET];
        memset(servIPs, 0, sizeof(uint8_t)*MDNS_MAX_SERVICES_PER_PACKET*5);
        memset(servTxt, 0, sizeof(uint8_t*)*MDNS_MAX_SERVICES_PER_PACKET);
         
        const uint8_t* ptrNamesCmp[MDNS_MAX_SERVICES_PER_PACKET];
        int ptrLensCmp[MDNS_MAX_SERVICES_PER_PACKET];
        uint8_t ptrNamesMatches[MDNS_MAX_SERVICES_PER_PACKET];
         
        uint8_t checkAARecords = 0;
        memset(ptrNames, 0, sizeof(uint8_t*)*MDNS_MAX_SERVICES_PER_PACKET);
         
        const uint8_t* servNames[2];
        uint8_t servNamePos[2];
        int servLens[2];
        uint8_t servMatches[2];
        uint8_t firstNamePtrByte = 0;
        uint8_t partMatched[2];
        uint8_t lastWasCompressed[2];
        uint8_t servWasCompressed[2];
         
		servNamePos[0] = servNamePos[1] = 0;
                  
		for (uint16_t i = 0; i < qCnt + aCnt + aaCnt + addCnt; i++) 
		{
            for (uint8_t j = 0; j < 2; j++) {
               if (NULL != _resolveNames[j]) {
                  servNames[j] = _resolveNames[j];
                  servLens[j] = strlen((const char*)_resolveNames[j]);
                  servMatches[j] = 1;
               } else {
                  servNames[j] = NULL;
                  servLens[j] = servMatches[j] = 0;
               }
            }
            
            for (uint8_t j = 0; j < MDNS_MAX_SERVICES_PER_PACKET; j++) {
               if (NULL != ptrNames[j]) {
                  ptrNamesCmp[j] = ptrNames[j];
                  ptrLensCmp[j] = strlen((const char*)ptrNames[j]);
                  ptrNamesMatches[j] = 1;
               }
            }
            
            partMatched[0] = partMatched[1] = 0;
            lastWasCompressed[0] = lastWasCompressed[1] = 0;
            servWasCompressed[0] = servWasCompressed[1] = 0;
            firstNamePtrByte = 0;
            tLen = 0;
                        
            do {
            	memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,1);
               	offset += 1;
               	rLen = buf[0];
               	tLen += 1;
            
               	if (rLen > 128) { // handle DNS name compression, kinda, sorta...

                	memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,1);
                  	offset += 1;

                  	for (uint8_t j = 0; j < 2; j++) {
                    	if (servNamePos[j] && servNamePos[j] != buf[0])
                        	servMatches[j] = 0;
                     	else
                        	servWasCompressed[j] = 1;
                     
                     	lastWasCompressed[j] = 1;
                  	}
               
                  	tLen += 1;
                  
                  	if (0 == firstNamePtrByte)
                    	firstNamePtrByte = buf[0];
               } else if (rLen > 0) {
               		if (i < qCnt)
                    	offset += rLen;
                	else {
                    	int tr = rLen, ir;
                     
                    	if (0 == firstNamePtrByte)
                        	firstNamePtrByte = offset - 1; // -1, since we already read length (1 byte)
               
                    	while (tr > 0) {
                        	ir = ((size_t)tr > sizeof(DNSHeader_t)) ? sizeof(DNSHeader_t) : tr;
                        	memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,ir);
                        	offset += ir;
                        	tr -= ir;
                  
                        	for (uint8_t j = 0; j < 2; j++) {
                           		if (!recordsFound[j] && servMatches[j] && servNames[j])
                              		servMatches[j] &= _matchStringPart(&servNames[j], &servLens[j], buf, ir);
                              	if (!partMatched[j])
                                	partMatched[j] = servMatches[j];
                              
                              	lastWasCompressed[j] = 0;
                        	}         
                        
                        	for (uint8_t j = 0; j < MDNS_MAX_SERVICES_PER_PACKET; j++) {
                           		if (NULL != ptrNames[j] && ptrNamesMatches[j]) {
                            		// only compare the part we have. this is incorrect, but good enough,
                            		// since actual MDNS implementations won't go here anyways, as they
                            		// should use name compression. This is just so that multiple Arduinos
                            		// running this MDNSResponder code should be able to find each other's
                            		// services.
                            		if (ptrLensCmp[j] >= ir) 
                                 		ptrNamesMatches[j] &= _matchStringPart(&ptrNamesCmp[j], &ptrLensCmp[j], buf, ir);
                           		}
                       		}
                     	}                    
                     
                     	tLen += rLen;
                  	}
               	}
            } while (rLen > 0 && rLen <= 128);
                        
            // if this matched a name of ours (and there are no characters left), then
            // check wether this is an A record query (for our own name) or a PTR record query
            // (for one of our services).
            // if so, we'll note to send a record
            if (i < qCnt)
               	offset += 4;
            else if (i >= qCnt) 
            {               
               	if (i >= qCnt + aCnt && !checkAARecords) break;
               
               	uint8_t packetHandled = 0;

               	memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,4);
               	offset += 4;
               	if (i < qCnt + aCnt) 
               	{
                  	for (uint8_t j = 0; j < 2; j++) 
                  	{
                    	if (0 == servNamePos[j])
                        	servNamePos[j] = offset - 4 - tLen;
                                      
                    	if (servNames[j] && ((servMatches[j] && 0 == servLens[j]) || (partMatched[j] && lastWasCompressed[j]) || (servWasCompressed[j] && servMatches[j]))) 
                        { 
                        	// somewhat handle compression by guessing                 
                        	if (buf[0] == 0 && buf[1] == ((0 == j) ? 0x01 : 0x0c) &&
                           		(buf[2] == 0x00 || buf[2] == 0x80) && buf[3] == 0x01) 
                           	{
                           		recordsFound[j] = 1;
                           		
                           		// this is an A or PTR type response. Parse it as such.
                           		memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,6);
                           		offset += 6;
                           		//uint32_t ttl = ntohl(*(uint32_t*)buf);
                           		uint16_t dataLen = ntohs(*(uint16_t*)&buf[4]);
                        
                           		if (0 == j && 4 == dataLen) 
                           		{
                              		// ok, this is the IP address. report it via callback.

                        	   		memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,4);
                              		_finishedResolvingName((char*)this->_resolveNames[0], (const byte*)buf);
                           		} 
                           		else if (1 == j) 
                           		{
                              		uint8_t k;
                              		for (k = 0; k < MDNS_MAX_SERVICES_PER_PACKET; k++)
                                 		if (NULL == ptrNames[k]) break;
                           
                              		if (k < MDNS_MAX_SERVICES_PER_PACKET) {
                                 		int l = dataLen - 2; // -2: data compression of service postfix
                              
                                 		uint8_t* ptrName = (uint8_t*)my_malloc(l);
                              
                                 		if (ptrName) {
                                			memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,1);
                                	 		memcpy((uint8_t*)ptrName, (uint16_t*)(ptr+offset+1) ,l-1);
                                 
                                    		if (buf[0] < l-1)
                                       			(void)ptrName[buf[0]]; // this catches uncompressed names
                                    		else
                                       			ptrName[l-1] = '\0';
                                    
                                    		ptrNames[k] = ptrName;
                                    		ptrOffsets[k] = (uint16_t)(offset);
 
                                    		checkAARecords = 1;
                                 		}
                              		}
                           		}
                           		offset += dataLen;
                           		packetHandled = 1;
                        	}
                     	}
                  	}
               	} 
               	else if (i >= qCnt + aCnt + aaCnt) 
               	{
                	// check whether we find a service description
                	if (buf[1] == 0x21) 
                	{
                    	for (uint8_t j = 0; j < MDNS_MAX_SERVICES_PER_PACKET; j++) 
                    	{
                        	if (ptrNames[j] && ((firstNamePtrByte && firstNamePtrByte == ptrOffsets[j]) || (0 == ptrLensCmp[j] && ptrNamesMatches[j]))) 
                        	{
                        		// we have found the matching SRV location packet to a previous SRV domain
                        		memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,6);
                           		offset += 6;

                           		//uint32_t ttl = ntohl(*(uint32_t*)buf);
                          		uint16_t dataLen = ntohs(*(uint16_t*)&buf[4]);

                           		if (dataLen >= 8) 
                           		{
                           			memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,8);
                              		ptrPorts[j] = ntohs(*(uint16_t*)&buf[4]);
                              
                              		if (buf[6] > 128) { // target is a compressed name
                                 		ptrIPs[j] = buf[7];
                              		} else { // target is uncompressed
                                 		ptrIPs[j] = offset+6;
                              		}
                           		}
                           		offset += dataLen;
                           		packetHandled = 1;
                           		break;
                        	}
                     	}
                 	} 
                 	else if (buf[1] == 0x10) 
                 	{ 
                 		// txt record
                     	for (uint8_t j = 0; j < MDNS_MAX_SERVICES_PER_PACKET; j++) 
                     	{
                        	if (ptrNames[j] && ((firstNamePtrByte && firstNamePtrByte == ptrOffsets[j]) || (0 == ptrLensCmp[j] && ptrNamesMatches[j]))) 
                        	{
                        		memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,6);
                           		offset += 6;

                           		//uint32_t ttl = ntohl(*(uint32_t*)buf);
                           		uint16_t dataLen = ntohs(*(uint16_t*)&buf[4]);
                        
                           		// if there's a content to this txt record, save it for delivery
                           		if (dataLen > 1 && NULL == servTxt[j]) 
                           		{
                              		servTxt[j] = (uint8_t*)my_malloc(dataLen+1);
                              		if (NULL != servTxt[j]) {
                              			memcpy((uint8_t*)servTxt[j], (uint16_t*)(ptr+offset) ,dataLen);
                              
                                 		// zero-terminate
                                 		servTxt[j][dataLen] = '\0';
                              		}
                           		}
                           		
                           		offset += dataLen;
                           		packetHandled = 1;
                           		break;
                        	}
                     	}
                  	} 
                  	else if (buf[1] == 0x01) 
                  	{ 
                  		// A record (IPv4 address)                     
                     	for (uint8_t j = 0; j < MDNS_MAX_SERVICES_PER_PACKET; j++) 
                     	{
                        	if (0 == servIPs[j][0]) 
                        	{
                           		servIPs[j][0] = firstNamePtrByte ? firstNamePtrByte : 255;
                           		
                           		memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset) ,6);
                           		offset += 6;

                           		uint16_t dataLen = ntohs(*(uint16_t*)&buf[4]);
                           		if (4 == dataLen) {
                        	  		memcpy((uint8_t*)&servIPs[j][1], (uint16_t*)(ptr+offset) ,4);
                           		}
                           		
                           		offset += dataLen;
                           		packetHandled = 1;
                           		break;
                        	}
                     	}
                  	}
               	}
               
               	// eat the answer
               	if (!packetHandled) {
            		offset += 4; // ttl
            		memcpy((uint8_t*)buf, (uint16_t*)(ptr+offset), 2);
            		offset += 2 + ntohs(*(uint16_t*)buf); // skip over content
               	}
            }
		}
         
    	// deliver the services discovered in this packet
        if (NULL != this->_resolveNames[1]) 
        {
        	char* typeName = (char*)this->_resolveNames[1];
            char* p = (char*)this->_resolveNames[1];
            while (*p && *p != '.') p++;
            *p = '\0';
            
            for (uint8_t i = 0; i < MDNS_MAX_SERVICES_PER_PACKET; i++)
            {
            	if (NULL == ptrNames[i]) continue;

				const uint8_t* ipAddr = NULL;
				const uint8_t* fallbackIpAddr = NULL;

				for (uint8_t j = 0; j < MDNS_MAX_SERVICES_PER_PACKET; j++) 
				{
					if (servIPs[j][0] == ptrIPs[i] || servIPs[j][0] == 255) {
						// the || part is such a hack, but it will work as long as there's only
						// one A record per MDNS packet. fucking DNS name compression.                     
						ipAddr = &servIPs[j][1];
						break;
					} 
					else if (NULL == fallbackIpAddr && 0 != servIPs[j][0]) {
						fallbackIpAddr = &servIPs[j][1];
					}
				}
		   
				// if we can't find a matching IP, we try to use the first one we found.
				if (NULL == ipAddr) ipAddr = fallbackIpAddr;
		   
				if (ipAddr && this->_serviceFoundCallback) {
					_serviceFoundCallback(typeName, _resolveServiceProto, (const char*)ptrNames[i], 
										  (const byte*)ipAddr, (unsigned short)ptrPorts[i], (const char*)servTxt[i]);
				}
            }
        	*p = '.';
        }
        
        for (uint8_t k = 0; k < MDNS_MAX_SERVICES_PER_PACKET; k++) 
        {
        	if (NULL != ptrNames[k]) {
            	my_free(ptrNames[k]);
               	if (NULL != servTxt[k])
                 	my_free(servTxt[k]);
            }
        }
	}

#endif // (defined(HAS_SERVICE_REGISTRATION) && HAS_SERVICE_REGISTRATION) || (defined(HAS_NAME_BROWSING) && HAS_NAME_BROWSING)

    my_free(udpBuffer);

errorReturn:

#if defined(_USE_MALLOC_) 
    if (NULL != dnsHeader)
        my_free(dnsHeader);
#endif
   
    IPAddress _remoteIP = remoteIP();
    
    // now, handle the requests
    for (uint8_t j = 0; j < NumMDNSServiceRecords + 2; j++) 
    {
        if (recordsAskedFor[j]) 
        {
            if (0 == j)
                (void)_sendMDNSMessage(&_remoteIP, xid, (int)MDNSPacketTypeMyIPAnswer, 0);
            else if (1 == j) {
                for (uint8_t k = 0; k < NumMDNSServiceRecords; k++)
                    recordsAskedFor[k + 2] = 1;
            } 
            else if (NULL != _serviceRecords[ j- 2])
                (void)_sendMDNSMessage(&_remoteIP, xid, (int)MDNSPacketTypeServiceRecord, j - 2);
        }
    }
   
    // if we were asked for our IPv6 address, say that we don't have any
    if (wantsIPv6Addr) {
        (void)_sendMDNSMessage(&_remoteIP, xid, (int)MDNSPacketTypeNoIPv6AddrAvailable, 0);
    }
    return statusCode;
}

void BonjourClass::run()
{
    unsigned long now = millis();
   
    // first, look for MDNS queries to handle
    (void)_processMDNSQuery();
   
    // are we querying a name or service? if so, should we resend the packet or time out?
    for (int i = 0; i < 2; i++) 
    {
        if (NULL == _resolveNames[i]) continue;
        
        // Hint: _resolveLastSendMillis is updated in _sendMDNSMessage
        if (now - _resolveLastSendMillis[i] > (uint32_t)((i == 0) ? MDNS_NQUERY_RESEND_TIME : MDNS_SQUERY_RESEND_TIME))
            (void)_sendMDNSMessage(NULL, 0, (i == 0) ? MDNSPacketTypeNameQuery : MDNSPacketTypeServiceQuery, 0);
      
        if (_resolveTimeouts[i] > 0 && now > _resolveTimeouts[i]) 
        {
            if (i == 0)
                _finishedResolvingName((char*)_resolveNames[0], NULL);
            else if (i == 1 && _serviceFoundCallback) 
            {
                char* typeName = (char*)_resolveNames[1];
                char* p = (char*)_resolveNames[1];
                while (*p && *p != '.') p++;
                *p = '\0';
                    
                _serviceFoundCallback(typeName, _resolveServiceProto, NULL, NULL, 0, NULL);
            }
               
            if (NULL != _resolveNames[i]) {
                my_free(_resolveNames[i]);
                _resolveNames[i] = NULL;
            }
        }
    }
   
    // now, should we re-announce our services again?
    unsigned long announceTimeOut = (((uint32_t)MDNS_RESPONSE_TTL/2)+((uint32_t)MDNS_RESPONSE_TTL/4));
    if ((now - _lastAnnounceMillis) > 1000 * announceTimeOut) 
    {
        for (int i = 0; i < NumMDNSServiceRecords; i++) {
            if (NULL == _serviceRecords[i]) continue;
            (void)_sendMDNSMessage(NULL, 0, (int)MDNSPacketTypeServiceRecord, i);
        }
      
        _lastAnnounceMillis = now;
    }
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::setBonjourName(const char* bonjourName)
{
    if (NULL == bonjourName || 0 == *bonjourName) 
        return 0;
         
    if (_bonjourName != NULL)
        my_free(_bonjourName);
   
    _bonjourName = (uint8_t*)my_malloc(strlen(bonjourName) + 7);
    if (NULL == _bonjourName)
        return 0;
   
    strcpy((char*)_bonjourName, bonjourName);
    strcat((char*)_bonjourName, MDNS_TLD);
    return 1;
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::addServiceRecord(const char* name, uint16_t port, MDNSServiceProtocol_t proto)
{
    return addServiceRecord(name, port, proto, NULL);
}

// return values:
// 1 on success
// 0 otherwise
int BonjourClass::addServiceRecord(const char* name, uint16_t port, MDNSServiceProtocol_t proto, const char* textContent)
{
    if (NULL == name || 0 == *name || 0 == port) return 0; 

    int status = 0;
    MDNSServiceRecord_t* record = NULL;
    
    for (uint8_t i = 0; i < NumMDNSServiceRecords; i++) 
    {
        if (NULL != _serviceRecords[i]) continue; // slot is not empty
        
        record = (MDNSServiceRecord_t*)my_malloc(sizeof(MDNSServiceRecord_t));
        if (NULL == record) break; // allocation has failed, no reason to retry
            
        record->name = record->textContent = NULL;
               
        record->name = (uint8_t*)my_malloc(strlen((char*)name));
        if (NULL == record->name)
            goto errorReturn;
               
        if (NULL != textContent && 0 != *textContent) {
            record->textContent = (uint8_t*)my_malloc(strlen((char*)textContent));
            if (NULL == record->textContent)
                goto errorReturn;
              
            strcpy((char*)record->textContent, textContent);
        }
           
        record->port = port;
        record->proto = proto;
        strcpy((char*)record->name, name);
               
        uint8_t* s = _findFirstDotFromRight(record->name);
        record->servName = (uint8_t*)my_malloc(strlen((char*)s) + 12);
        if (record->servName) {
            strcpy((char*)record->servName, (const char*)s);
            const uint8_t* srv_type = _postfixForProtocol(proto);
            if (srv_type)
                strcat((char*)record->servName, (const char*)srv_type);
        }
            
        _serviceRecords[i] = record;
        status = (MDNSSuccess == _sendMDNSMessage(0, 0, (int)MDNSPacketTypeServiceRecord, i));
        break;
    }

    return status;

errorReturn:
    if (NULL != record) {
        if (NULL != record->name)
            my_free(record->name);
        if (NULL != record->servName)
            my_free(record->servName);
        if (NULL != record->textContent)
            my_free(record->textContent);
      
        my_free(record);
    }
    return 0;
}

void BonjourClass::_removeServiceRecord(int idx)
{
   if (NULL != _serviceRecords[idx]) 
   {
      (void)_sendMDNSMessage(0, 0, (int)MDNSPacketTypeServiceRecordRelease, idx);
      
      if (NULL != _serviceRecords[idx]->textContent)
         my_free(_serviceRecords[idx]->textContent);
      
      if (NULL != _serviceRecords[idx]->servName)
         my_free(_serviceRecords[idx]->servName);
      
      my_free(_serviceRecords[idx]->name);
      my_free(_serviceRecords[idx]);
      
      _serviceRecords[idx] = NULL;
   }
}

void BonjourClass::removeServiceRecord(uint16_t port, MDNSServiceProtocol_t proto)
{
   removeServiceRecord(NULL, port, proto);
}

void BonjourClass::removeServiceRecord(const char* name, uint16_t port, MDNSServiceProtocol_t proto)
{
	for (uint8_t i = 0; i < NumMDNSServiceRecords; i++)
		if (port == _serviceRecords[i]->port && proto == _serviceRecords[i]->proto &&
			(NULL == name || 0 == strcmp((char*)_serviceRecords[i]->name, name))) {
			_removeServiceRecord(i);
			break;
		}
}

void BonjourClass::removeAllServiceRecords()
{
	for (uint8_t i = 0; i < NumMDNSServiceRecords; i++)
		_removeServiceRecord(i);
}

void BonjourClass::_writeDNSName(const uint8_t* name, uint16_t* pPtr, uint8_t* buf, int bufSize, int zeroTerminate)
{
	uint16_t ptr = *pPtr;
	uint8_t* p1 = (uint8_t*)name, *p2, *p3;
	int i, c, len;
   
	while(*p1) {
		c = 1;
		p2 = p1;
		while (0 != *p2 && '.' != *p2) { p2++; c++; };

		p3 = buf;
		i = c;
		len = bufSize - 1;
      	*p3++ = (uint8_t)--i;
		while (i-- > 0) {
			*p3++ = *p1++;

			if (--len <= 0) {
            	write((uint8_t*)buf, bufSize);
            	ptr += bufSize;
            	len = bufSize;
            	p3 = buf;
         	}
      	}

		while ('.' == *p1)
			p1++;

		if (len != bufSize) {
			write((uint8_t*)buf, bufSize-len);
			ptr += bufSize-len;
		}
   	}
   
   	if (zeroTerminate) {
      	buf[0] = 0;
      	write((uint8_t*)buf, 1);
      	ptr += 1;
   	}
     
   	*pPtr = ptr;
}

void BonjourClass::_writeMyIPAnswerRecord(uint16_t* pPtr, uint8_t* buf, int bufSize)
{
	uint16_t ptr = *pPtr;
   
	_writeDNSName(_bonjourName, &ptr, buf, bufSize, 1);

	buf[0] = 0x00;
	buf[1] = 0x01;
	buf[2] = 0x80; // cache flush: true
	buf[3] = 0x01;
	write((uint8_t*)buf, 4);
	ptr += 4;

	*((uint32_t*)buf) = htonl(MDNS_RESPONSE_TTL);
	*((uint16_t*)&buf[4]) = htons(4);      // data length

	IPAddress myIp = WiFi.localIP();
	buf[6] = myIp[0];                      // our IP address
	buf[7] = myIp[1];
   	buf[8] = myIp[2];
   	buf[9] = myIp[3];

	write((uint8_t*)buf, 10);
	ptr += 10;
   
	*pPtr = ptr;
}

void BonjourClass::_writeServiceRecordName(int recordIndex, uint16_t* pPtr, uint8_t* buf, int bufSize, int tld)
{
	uint16_t ptr = *pPtr;
      
	uint8_t* name = tld ? _serviceRecords[recordIndex]->servName : _serviceRecords[recordIndex]->name;
   
	_writeDNSName(name, &ptr, buf, bufSize, tld);
   
	if (0 == tld) {
		const uint8_t* srv_type = _postfixForProtocol(_serviceRecords[recordIndex]->proto);
   
		if (NULL != srv_type) {
			srv_type++; // eat the dot at the beginning
			_writeDNSName(srv_type, &ptr, buf, bufSize, 1);
		}
	}
   
	*pPtr = ptr;
}

void BonjourClass::_writeServiceRecordPTR(int recordIndex, uint16_t* pPtr, uint8_t* buf, int bufSize, uint32_t ttl)
{
	uint16_t ptr = *pPtr;

	_writeServiceRecordName(recordIndex, &ptr, buf, bufSize, 1);
   
	buf[0] = 0x00;
	buf[1] = 0x0c;    // PTR record
	buf[2] = 0x00;    // no cache flush
	buf[3] = 0x01;    // class IN
   
	// ttl
	*((uint32_t*)&buf[4]) = htonl(ttl);
   
	// data length (+13 = "._tcp.local" or "._udp.local" + 1  byte zero termination)
	*((uint16_t*)&buf[8]) = htons(strlen((char*)_serviceRecords[recordIndex]->name) + 13);

	write((uint8_t*)buf, 10);
	ptr += 10;
   
	_writeServiceRecordName(recordIndex, &ptr, buf, bufSize, 0);
	
	*pPtr = ptr;
}

uint8_t* BonjourClass::_findFirstDotFromRight(const uint8_t* str)
{
	const uint8_t* p = str + strlen((char*)str);
	while (p > str && '.' != *p--);
	return (uint8_t*)&p[2];
}

int BonjourClass::_matchStringPart(const uint8_t** pCmpStr, int* pCmpLen, const uint8_t* buf, int dataLen)
{
	int matches = 1;

	if (*pCmpLen >= dataLen)
		matches &= (0 == memcmp(*pCmpStr, buf, dataLen));
	else
		matches = 0;

	*pCmpStr += dataLen;
	*pCmpLen -= dataLen;
	if ('.' == **pCmpStr)
		(*pCmpStr)++, (*pCmpLen)--;

	return matches;
}

const uint8_t* BonjourClass::_postfixForProtocol(MDNSServiceProtocol_t proto)
{
	const uint8_t* srv_type = NULL;
	switch(proto) {
		case MDNSServiceTCP:
			srv_type = (uint8_t*)"._tcp" MDNS_TLD;
			break;
		case MDNSServiceUDP:
			srv_type = (uint8_t*)"._udp" MDNS_TLD;
			break;
	}
	return srv_type;
}

void BonjourClass::_finishedResolvingName(char* name, const byte ipAddr[4])
{   
	if (NULL != _nameFoundCallback) {
		if (NULL != name) {
			uint8_t* n = _findFirstDotFromRight((const uint8_t*)name);
			*(n-1) = '\0';
		}
   
		_nameFoundCallback((const char*)name, ipAddr);
	}

	my_free(_resolveNames[0]);
	_resolveNames[0] = NULL;
}

BonjourClass Bonjour;