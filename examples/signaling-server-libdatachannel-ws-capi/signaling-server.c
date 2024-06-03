/**
 * signaling server example for libdatachannel
 * Copyright (c) 2024 Tim Schneider
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <rtc/rtc.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsmn.h"

#define MAX_PATH_LENGTH 1024
#define MAX_CLIENTS 1024
#define MAX_JSON_TOKENS 1024 /* We expect no more than 1024 tokens */

struct ConnectedClients 
{ 
    int ids[MAX_CLIENTS];
    char* client[MAX_CLIENTS];
} my_ConnectedClients;

char* last_path_segment(char* path,  const char* delim){
    char * last_segment = NULL;
    char * segment = strtok(path, delim);
    if(segment == NULL) return path;
    while(segment != NULL){
        last_segment = (char*)segment;
        segment = strtok(NULL, delim);
    }
    return last_segment;
}

static void RTC_API my_rtcOpenCallbackFunc(int id, void *ptr){
    fprintf(stdout, "websocket #%d opened\n", id);
    
    char path[MAX_PATH_LENGTH];
    memset(path, 0, MAX_PATH_LENGTH);
    const int size = rtcGetWebSocketPath(id, path, MAX_PATH_LENGTH);
    path[size] = 0;
    fprintf(stdout, "path:%s\n", path);

    const char* user = last_path_segment(path, "/");
    fprintf(stdout, "new user login:%s\n", user);
    
    // login user
    for (size_t i = 0; i <= MAX_CLIENTS; i++)
    {
        if(i == MAX_CLIENTS){
            const char MSG[] = "Too many clients connected already, try again after some Client leaves\n";
            fprintf(stderr, MSG);
            rtcSendMessage(id, MSG, -(int)sizeof(MSG)); // negative size to send the message as text (not binary)
            rtcClose(id);
            return;
        }
        
        if(my_ConnectedClients.ids[i] <= 0 ){
            my_ConnectedClients.ids[i] = id;
            const size_t len = strlen(user);
            my_ConnectedClients.client[i] = (char*) malloc(len+1);
            strncpy(my_ConnectedClients.client[i], user, len);
            ((char*)my_ConnectedClients.client[i])[len]=0; 
            break;
        }
    }
}

static void RTC_API my_rtcClosedCallbackFunc(int id, void* ptr){
    fprintf(stdout, "websocket #%d closed\n", id);
    for (size_t i = 0; i < MAX_CLIENTS; i++)
    {
        if(my_ConnectedClients.ids[i] != id) continue;

        my_ConnectedClients.ids[i] = -1;
        free(my_ConnectedClients.client[i]);
        return;
    }
    rtcDeleteWebSocket(id);
}

static void RTC_API my_rtcErrorCallbackFunc(int id, const char *error, void *ptr){
    fprintf(stderr, "websocket #%d error: %s\n", id, error);
    rtcClose(id);
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
  if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
      strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
    return 0;
  }
  return -1;
}

static void RTC_API my_rtcMessageCallback(int id, const char *message, int size, void *ptr){
    bool is_binary = size > 0;
    int message_len = abs(size);
    fprintf(stdout, "message (%s) from websocket #%d (len %d):%s\n", is_binary?"binary":"text", id, message_len, message);
    
    int i = 0;
    int r = 0;
    jsmn_parser p;
    jsmntok_t t[MAX_JSON_TOKENS];
    
    jsmn_init(&p);
    r = jsmn_parse(&p, message, message_len, t, sizeof(t) / sizeof(t[0]));

    /* Assume the top-level element is an object */
    if (r < 1 || t[0].type != JSMN_OBJECT) {
        fprintf(stderr, "Object expected: %.*s\n", message_len, message);
        return;
    }

    int i_dest = -1;
    int i_src = -1;
    const char* dest_id = NULL;
    int len_dest_id = 0; 

    /* Loop over all keys of the root object */
    for (i = 1; i < r; i++) {
        if (jsoneq(message, &t[i], "id") == 0) {
            /* fetch string value */
            dest_id = message + t[i + 1].start;
            len_dest_id = t[i + 1].end - t[i + 1].start; 
            for (int i = 0; i < MAX_CLIENTS; i++)
            {
                if(my_ConnectedClients.ids[i] < 0) continue;
                if(my_ConnectedClients.ids[i] == id) i_src = i;
                if(i_dest < 0 && strncmp(my_ConnectedClients.client[i], dest_id, len_dest_id) == 0) i_dest = i;
                if(i_dest >= 0 && i_src >= 0) break;
            }

            i++;
        }
    }

    if(i_dest < 0){
        fprintf(stderr, "No client %.*s connected.\n", len_dest_id, dest_id);
        return;
    }
    if(i_src >= 0){
        // in message, replace dest_id with src_id and send data to websocket correspondig to dest_id

        const char* src_id_ptr = my_ConnectedClients.client[i_src];
        const int src_id_len = (int)strlen(src_id_ptr);
        
        const int new_message_len = message_len-len_dest_id+src_id_len;
        char* new_message = (char*)malloc(new_message_len);
        const int o_id = (int)(dest_id-message); // offset id value
        strncpy(new_message, message, o_id); // characters before id
        strncpy(new_message+o_id, src_id_ptr, src_id_len); // insert src_id
        strncpy(new_message+o_id+src_id_len, dest_id+len_dest_id, message_len-o_id-len_dest_id); // characters after id

        fprintf(stdout, "message (%s) to websocket #%d (len %d): %.*s", 
            is_binary?"binary":"text", my_ConnectedClients.ids[i_dest], 
            (int)new_message_len, (int)new_message_len, new_message);
        int sent = rtcSendMessage(my_ConnectedClients.ids[i_dest], 
            new_message, (int) is_binary?new_message_len:-(new_message_len)); // negative message length indicates text-message instead of binary messsage 
        free(new_message);
    }
    // if either no json or no matching client found
}

static void RTC_API my_rtcWebSocketClientCallbackFunc(int wsserver, int ws, void *user_ptr){
    fprintf(stdout, "websocket #%d connected to websocket-server %d\n",  ws, wsserver);

    rtcSetUserPointer(ws, &my_ConnectedClients);
    rtcSetOpenCallback(ws, my_rtcOpenCallbackFunc);
    rtcSetMessageCallback(ws, my_rtcMessageCallback);
    rtcSetClosedCallback(ws, my_rtcClosedCallbackFunc);
    rtcSetErrorCallback(ws, my_rtcErrorCallbackFunc);
}

int main(int argc, char* argv[])
{
    rtcWsServerConfiguration config;
    config.port = 8000;
    config.enableTls = false;
    config.certificatePemFile = NULL;
    config.keyPemFile = NULL;
    config.keyPemPass = NULL;
    config.bindAddress = NULL;

    // Check command line arguments.
    for (int i = 1; i < argc; i++)
    {
        if( strcmp(argv[i], "--help") == 0 ){
            const size_t len = strlen(argv[0]);
            char* path = (char*) malloc(len+1);
            strcpy(path, argv[0]);
            path[len] = 0;

            char* app_name = NULL;
            app_name = last_path_segment(path, "\\/");
            fprintf( stderr, 
                "Usage: %s [-p <port>] [-a <bind-address>] [--enable-tls] [--certificatePemFile <file>] [--keyPemFile <keyPemFile>] [--keyPemPass <pass>]\n" 
                "Example:\n"
                "    %s -p 8000 -a 127.0.0.1 \n"
                , app_name, app_name);
            free(path);
            return EXIT_FAILURE;
        }
        if( strcmp(argv[i], "-p") == 0 ){
            config.port = atoi(argv[++i]);
            continue;
        }
        if( strcmp(argv[i], "-a") == 0 ){
            config.bindAddress = argv[++i];
            continue;
        }
        if( strcmp(argv[i], "--enable-tls") == 0 ){
            config.enableTls = true;
            continue;
        }
        if( strcmp(argv[i], "--certificatePemFile") == 0 ){
            config.certificatePemFile = argv[++i];
            continue;
        }
        if( strcmp(argv[i], "--keyPemFile") == 0 ){
            config.keyPemFile = argv[++i];
            continue;
        }
        if( strcmp(argv[i], "--keyPemPass") == 0 ){
            config.keyPemPass = argv[++i];
            continue;
        }
    }

    memset(&my_ConnectedClients, 0, sizeof(my_ConnectedClients));

    int wsserver = rtcCreateWebSocketServer(&config, my_rtcWebSocketClientCallbackFunc);

    if(wsserver < 0){
        fprintf(stderr, "Error creating WebsocketServer");
        return EXIT_FAILURE;
    }

    fprintf(stdout, "Started signaling-server on %s://%s:%d\n", config.enableTls?"wss":"ws", config.bindAddress, config.port);

    printf("press any key to exit...\n");
    int c = getchar();
    rtcDeleteWebSocketServer(wsserver);

    return EXIT_SUCCESS;
}