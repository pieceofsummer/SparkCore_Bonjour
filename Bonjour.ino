#include "Bonjour.h"

#define SERVICE_NAME "myspark"

// append TXT record entry
void txt_add(char *&txt, const char *key, const char *value)
{
    uint8_t keyLen = strlen(key);
    uint8_t valueLen = strlen(value);
    if (keyLen + valueLen > 254) return;
    
    *(txt++) = keyLen + valueLen + 1;
    
    memcpy(txt, key, keyLen);
    txt += keyLen;
    
    *(txt++) = '=';
    
    memcpy(txt, value, valueLen);
    txt += valueLen;
    *txt = 0;
}

void setup()
{
	if (Bonjour.begin(SERVICE_NAME))
    {
        // Bonjour has successfully started, now setup accessory discovery record
        
        char szTXT[72]; // don't make it too long
        char *txt = szTXT;
        txt_add(txt, "url", "spark.io");
        
        Bonjour.addServiceRecord(SERVICE_NAME "._http", 80, MDNSServiceTCP, szTXT);
    }
}

void loop()
{
	while (true)
	{
		Bonjour.run();
	}
}