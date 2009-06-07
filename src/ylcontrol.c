/****************************************************************************
 *
 *    File: ylcontrol.c
 *
 *    Copyright (C) 2006, 2007    Thomas Reitmayr <treitmayr@yahoo.com>
 *
 ****************************************************************************
 *
 *    This file is part of Yeaphone.
 *
 *    Yeaphone is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.    See the
 *    GNU Library General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/kd.h>


#include <linphone/linphonecore.h>
#include <osipparser2/osip_message.h>
#include "yldisp.h"
#include "lpcontrol.h"
#include "ylcontrol.h"
#include "ypconfig.h"

#ifdef DMALLOC
#include <dmalloc.h>
#endif


#define MAX_NUMBER_LEN 32

#ifndef CLOCK_TICK_RATE
#define CLOCK_TICK_RATE 1193180
#endif


typedef struct ylcontrol_data_s {
    int evfd;
    
    int kshift;
    int pressed;
    
    int prep_store;
    int prep_recall;
    
    char dialnum[MAX_NUMBER_LEN];
    char callernum[MAX_NUMBER_LEN];
    char dialback[MAX_NUMBER_LEN];
    
    char *intl_access_code;
    char *country_code;
    char *natl_access_code;
    
    pthread_t control_thread;
} ylcontrol_data_t;

ylcontrol_data_t ylcontrol_data;
//--------------------------

struct Display {
    char text[MAX_NUMBER_LEN];
};

enum EventType {
PAINT=1000,
INIT,
TIMER,
KEY,
VOIP
};

int eventPipe[2];
int console_fd;

enum KeyCode {K_UP=103,K_IN=105,K_OUT=106,K_DOWN=108};


enum PipeEventType {
TAP_TIMER=2000,
VOIP_PIPE
};

struct VoipEvent {
    gstate_t power, call, reg;
    char message[256];
};

struct PipeEvent {
    enum PipeEventType type;
    struct VoipEvent voip;
};

struct Event {
    enum EventType type;
    struct {
        int code, value;
        char c;
    } key;
    struct VoipEvent voip;
    struct {
        struct Display *display;
        int repaint;
    } paint;
    struct {
        long long smallestNext;
    } timer;
};

enum NumberEditFlags {NE_NONE=0,NE_USE_UP_DOWN=1};

struct NumberEdit {
    char text[128];
    char initialNumber[128];
    int maxLen;
    int number;
    int len;
    enum NumberEditFlags flags;
};

struct Cursor {
    long long nextBlink;
    char cursor;
};

enum MainPanelItem {NONE, DIAL_PANEL, MENU_PANEL, CALL_PANEL, SEARCH_PANEL, HISTORY_PANEL, VOIP_STATUS_PANEL, EXIT_PANEL};

struct HistoryItem {
    char number[64];
    int isOutgoing;
    long long callStarted,callEnded;
};

enum MelodyType {MELODY_DEFAULT='D', MELODY_SPEAKER='S'};

struct MelodyItem {
    float freq;
    int delay;
};

struct Melody {
    enum MelodyType type;
    int size;
    struct MelodyItem *items;
};

struct MelodyPlayer {
    int pos;
    struct Melody *melody;
    long long timer;
};

struct CallPanel {
    int callState, connected;
    long long callTimer;
    char incomingNumber[32];
    int incomingKnown;
    struct HistoryItem callInfo;
    struct Dictionary *dictionary;
    struct MelodyPlayer melodyPlayer;
};

struct DialPanel {
    struct NumberEdit numberEdit;
    struct Cursor cursor;
    struct CallPanel *callPanel;
    enum MainPanelItem switchTo;
};

enum MenuItem {SEARCH_ITEM, HISTORY_ITEM, VOIP_STATUS_ITEM};
struct {
    const char *desc;
    enum MainPanelItem panel;
} menuItemDesc[] = { 
{"search", SEARCH_PANEL},
{"history", HISTORY_PANEL},
{"voip status", VOIP_STATUS_PANEL},
{"exit", EXIT_PANEL}
};

const char *HISTORY_FILE=".yeaphone_history";

struct MenuPanel {
    enum MenuItem selected;
    enum MainPanelItem switchTo;
};


struct SearchItem {
    char name[13];
    char number[13];
    struct Melody melody;
};

#define DICTIONARY_SIZE 100

struct Dictionary {
    struct Melody defaultMelody;
    struct SearchItem items[DICTIONARY_SIZE];
};

struct SearchPanel {
    struct Dictionary dictionary;
    struct NumberEdit selected;
    enum MainPanelItem switchTo;
    struct DialPanel *dialPanel;
};


#define HISTORY_SIZE 100

struct HistoryItems {
    struct HistoryItem items[HISTORY_SIZE];
    int size;
};

struct HistoryPanel {
    int selected;
    struct HistoryItems items;
    enum MainPanelItem switchTo;
    struct DialPanel *dialPanel;
};
    
struct VoipStatusPanel {
    struct VoipEvent voip;
    enum MainPanelItem switchTo;
};

struct MainPanel {
    enum MainPanelItem enabled;
    struct SearchPanel searchPanel;
    struct DialPanel dialPanel;
    struct CallPanel callPanel;
    struct MenuPanel menuPanel;
    struct HistoryPanel historyPanel;
    struct VoipStatusPanel voipStatusPanel;
};

struct EnumDesc {
    int state;
    const char *desc;
};

struct EnumDesc voipStatusDesc[] = {
        {GSTATE_REG_OK, "registered"},
        {GSTATE_REG_FAILED, "failed"},
        {0,0}
    };


void beepSpeaker(float freq);

void extract_callernum(char *incomingNumber, const char *line);

void sendPipeEvent(struct PipeEvent *p) {
    fprintf(stderr,"before send %i %i\n",eventPipe[0], eventPipe[1]);
    write(eventPipe[1], p, sizeof(*p));
    fprintf(stderr,"after send\n");
}

const char *getEnumDesc(int state, struct EnumDesc *desc) {
    int i;
    for(i=0;desc[i].desc != 0;i++) {
        if (state == desc[i].state) return desc[i].desc;
    }
    return "";
}

long long timevalToLL(struct timeval *v) {
    return ((long long)v->tv_sec)*1000000 + v->tv_usec;
}

void llToTimeval(long long t, struct timeval *v) {
    v->tv_sec = t/1000000;
    v->tv_usec = t % 1000000;
}

long long now() {
    struct timeval tv;
    assert(gettimeofday(&tv, 0)==0);
    return timevalToLL(&tv);
}

void initTimerAfter(long long period, long long *target) {
    struct PipeEvent e;
    memset(&e,0,sizeof(e));
    *target = now() + period;
    e.type = TAP_TIMER;
    sendPipeEvent(&e);
}


int checkTimePassed(long long *t, struct Event *event) {
    if (*t == 0) return 0;
    if (*t<now()) {
        *t=0;
        return 1;
    } else {
        if (event->timer.smallestNext > *t) {
            event->timer.smallestNext = *t;
        }
        return 0;
    }
}


void melodyStart(struct MelodyPlayer *player, struct Melody *melody) {
    player->pos = 0;
    player->melody = melody;
    initTimerAfter(0, &player->timer);
}

void melodyTimerEvent(struct MelodyPlayer *player, struct Event *event) {
    if (checkTimePassed(&player->timer, event)) {
        beepSpeaker(player->melody->items[player->pos].freq);
        initTimerAfter((long long) player->melody->items[player->pos].delay * 1000, &player->timer);
        player->pos = (player->pos + 1) % player->melody->size;
    }
}

void melodyStop(struct MelodyPlayer *player) {
    player->timer = 0;
    beepSpeaker(0);
}

void cursorEvent(struct Event *event, struct Cursor *cursor) {
    switch (event->type) {
        case INIT: {
            cursor->cursor = '_';
            initTimerAfter(500000,&cursor->nextBlink);
        } break;
        case TIMER: {
            if (checkTimePassed(&cursor->nextBlink, event)) {
                initTimerAfter(500000,&cursor->nextBlink);
                cursor->cursor = '_' == cursor->cursor ? ' ' : '_';
                event->paint.repaint=1;
            }
        } break;
                 default:break;
    }
}

void numberEditEvent(struct Event *event, struct NumberEdit *numberEdit) {
    switch (event->type) {
        case INIT: {
            numberEdit->len=strlen(numberEdit->initialNumber);
            strcpy(numberEdit->text, numberEdit->initialNumber);
            numberEdit->initialNumber[0]=0;
        } break;
        case PAINT: {
            //snprintf(event->paint.display->text, 12, "%s_", numberEdit->text);
            numberEdit->number = atoi(numberEdit->text);
            fprintf(stderr,"number:%i\n",numberEdit->number);
        } break;
        case KEY: {
            if (event->key.value) {
                if (event->key.c >= '0' && event->key.c <= '9' && (numberEdit->maxLen == 0 || numberEdit->maxLen > numberEdit->len)) {
                    numberEdit->text[numberEdit->len] = event->key.c;
                    numberEdit->len ++;
                    event->paint.repaint=1;
                } else if (event->key.c == '^') {
                    if (numberEdit->len > 0) numberEdit->len --;
                    event->paint.repaint=1;
                } else if (event->key.c == '!') {
                    numberEdit->len = 0;
                    event->paint.repaint=1;
                } else if (numberEdit->flags == NE_USE_UP_DOWN) {
                    numberEdit->number = atoi(numberEdit->text);
                    fprintf(stderr,"number:%i text:%s\n",numberEdit->number, numberEdit->text);
                            
                    if (event->key.code == K_UP) {
                        numberEdit->number--;
                        if (numberEdit->number < 0) numberEdit->number = 0;
                        event->paint.repaint=1;
                    } else if (event->key.code == K_DOWN) {
                        numberEdit->number++;
                        event->paint.repaint=1;
                    }
                    fprintf(stderr,"number:%i text:%s\n",numberEdit->number, numberEdit->text);
                    {
                        int l = numberEdit->maxLen;
                        if (l == 0) l = 12;
                        fprintf(stderr,"l:%i\n",l);
                        snprintf(numberEdit->text, l+1, "%i", numberEdit->number);
                        numberEdit->len = strlen(numberEdit->text);
                    }
                    fprintf(stderr,"number:%i text:%s\n",numberEdit->number, numberEdit->text);
                }
                fprintf(stderr,"code:%i %i\n",event->key.code, event->key.value);
                numberEdit->text[numberEdit->len]=0;
            }
                 } break;
                 default:break;
    }
}

void voipStatusPanelEvent(struct Event *event, struct VoipStatusPanel *voipStatus) {
    switch (event->type) {
        case INIT: {
        } break;
        case PAINT: {
            snprintf(event->paint.display->text, 13, "%s", getEnumDesc(voipStatus->voip.reg, voipStatusDesc));
        } break;
        case KEY: {
            if (event->key.value) {
                voipStatus->switchTo = DIAL_PANEL;
            }
        } break;
        default:break;
    }
    
}


void dialPanelEvent(struct Event *event, struct DialPanel *dialPanel) {
    numberEditEvent(event, &dialPanel->numberEdit);
    cursorEvent(event, &dialPanel->cursor);

    switch (event->type) {
        case INIT: {
            yldisp_show_date();
        } break;
        case PAINT: {
            snprintf(event->paint.display->text, 13, "%s%c", dialPanel->numberEdit.text, dialPanel->cursor.cursor);
        } break;
        case KEY: {
            if (event->key.value) {
                if (event->key.c == '@') {
                    lpstates_submit_command(LPCOMMAND_CALL, dialPanel->numberEdit.text);
                    strcpy(dialPanel->callPanel->callInfo.number, dialPanel->numberEdit.text);
                }
                if (event->key.code == K_UP || event->key.code == K_DOWN) {
                    dialPanel->switchTo = MENU_PANEL;
                }
            }
        } break;
        default:break;
    }
}

const char *dictionaryFileName = ".yeaphone_dict";


void truncateStr(char *str) {
    int j=strlen(str)-1;
    while (j>=0 && (str[j] == ' ' || str[j] == '\t' || str[j]=='\n' || str[j]=='\r')) {
        str[j]=0;
        j--;
    }
}

void loadMelody(struct Melody *melody, FILE *f) {
    char buf[1024];
    fscanf(f, "%s", buf);
    melody->type = buf[0];
    if (melody->type == MELODY_SPEAKER) {
        struct MelodyItem melodyItems[1024];
        melody->size = 0;
        while (1) {
            fscanf(f, "%f", &melodyItems[melody->size].freq);
            if (melodyItems[melody->size].freq == -1) break;
            fscanf(f, "%i", &melodyItems[melody->size].delay);
            melody->size ++;
        }
        melody->items = (struct MelodyItem *) malloc(sizeof(struct MelodyItem) * melody->size);
        memcpy(melody->items, melodyItems, sizeof(struct MelodyItem) * melody->size);
    }
    //fscanf(f, "\n");
    fgets(buf,1024,f);
}

int loadDictionaryItem(struct SearchItem *item, FILE *f) {
    char buf[1024];
    //fgets(buf,1024,f);
    if (fscanf(f, "%[^;];", buf) != 1) return 0;
    snprintf(item->name, 13, "%s", buf);
    truncateStr(item->name);
    fscanf(f, "%[^;];", buf);
    snprintf(item->number, 13, "%s", buf);
    truncateStr(item->number);

    loadMelody(&item->melody, f);

//                int j = strlen(buf)-1;
//                while (j>0 && buf[j]!=';') j--;
//                buf[j]=0;
//                snprintf(item->name, 13, "%s", buf);
//                snprintf(items->number, 13, "%s", buf+j+1);
//                truncateStr(items->name);
//                truncateStr(items->number);
                fprintf(stderr,"loaded %s,%s,%c\n",item->name,item->number,item->melody.type);
    return 1;
}

void loadDictionary(struct Dictionary *dictionary) {
    int i;
    FILE *f;
    struct SearchItem *items = dictionary->items;
    memset(items, 0, sizeof(*items)*DICTIONARY_SIZE);
    f = fopen(dictionaryFileName, "r");
    fprintf(stderr,"loading %p\n",f);
    if (f) {
        loadMelody(&dictionary->defaultMelody, f);
        for (i=0;i<DICTIONARY_SIZE;i++) {
            if (!loadDictionaryItem(items+i, f)) break;    
        }
        fclose(f);
    }
}

void saveDictionary(struct SearchItem *items) {
    int i;
    FILE *f;
    f = fopen(dictionaryFileName, "w");
    for (i=0;i<DICTIONARY_SIZE;i++) {
        fprintf(f, "%s %s\n", items[i].name, items[i].number);
    }
    fclose(f);
}

void searchPanelEvent(struct Event *event, struct SearchPanel *searchPanel) {
    numberEditEvent(event, &searchPanel->selected);

    switch (event->type) {
        case INIT: {
            searchPanel->selected.maxLen = 2;
            searchPanel->selected.flags = NE_USE_UP_DOWN;
        } break;
        case PAINT: {
            snprintf(event->paint.display->text, 13, "%2i %s", searchPanel->selected.number, searchPanel->dictionary.items[searchPanel->selected.number].name);
        } break;
        case KEY: {
            if (event->key.value) {
                if (event->key.code == K_UP) {
                    event->paint.repaint=1;
                } else if (event->key.code == K_DOWN) {
                    event->paint.repaint=1;
                } else if (event->key.c == '@') {
                    searchPanel->switchTo = DIAL_PANEL;
                    strcpy(searchPanel->dialPanel->numberEdit.initialNumber, searchPanel->dictionary.items[searchPanel->selected.number].number);
                } else if (event->key.c == '!') {
                    searchPanel->switchTo = DIAL_PANEL;
                }
            }
        } break;
        default:break;
    }
}

void loadHistory(struct HistoryItems *items) {
    FILE *f = fopen(HISTORY_FILE, "rb");
    items->size = 0;
    fprintf(stderr,"loadHistory 1\n");
    if (f) {
        fseek(f, -HISTORY_SIZE * sizeof(struct HistoryItem), SEEK_END);
        items->size = fread(items->items, sizeof(*items->items), HISTORY_SIZE, f);
        fprintf(stderr,"loadHistory 2 items:%i\n",items->size);
    }
}

void historyPanelEvent(struct Event *event, struct HistoryPanel *historyPanel) {
    switch (event->type) {
        case INIT: {
            loadHistory(&historyPanel->items);
            historyPanel->selected = historyPanel->items.size - 1;
            if (historyPanel->items.size == 0) {
                historyPanel->switchTo = DIAL_PANEL;
            }
        } break;
        case PAINT: {
            if (historyPanel->items.size > 0) {
                snprintf(event->paint.display->text, 13, "%2i%c%s", historyPanel->selected, historyPanel->items.items[historyPanel->selected].isOutgoing ? ']' : '[', historyPanel->items.items[historyPanel->selected].number);
            }
        } break;
        case KEY: {
            fprintf(stderr,"key %c\n",event->key.c);
            if (event->key.value) {
                if (event->key.code == K_UP) {
                    historyPanel->selected --;
                    event->paint.repaint=1;
                } else if (event->key.code == K_DOWN) {
                    historyPanel->selected ++;
                    event->paint.repaint=1;
                } else if (event->key.c == '@') {
                    historyPanel->switchTo = DIAL_PANEL;
                    strcpy(historyPanel->dialPanel->numberEdit.initialNumber, historyPanel->items.items[historyPanel->selected].number);
                } else if (event->key.c == '!') {
                    fprintf(stderr,"switchTo DIAL_PANEL\n");
                    historyPanel->switchTo = DIAL_PANEL;
                }
                if (historyPanel->items.size > 0) {
                    historyPanel->selected = (historyPanel->selected + historyPanel->items.size) % historyPanel->items.size;
                }
            }
        } break;
        default:break;
    }
}

void menuPanelEvent(struct Event *event, struct MenuPanel *menuPanel) {
    switch (event->type) {
        case INIT: {
            menuPanel->selected = 0;
        } break;
        case PAINT: {
            snprintf(event->paint.display->text, 13, "%s", menuItemDesc[menuPanel->selected].desc);
        } break;
        case KEY: {
            if (event->key.value) {
                if (event->key.code == K_UP) {
                    menuPanel->selected--;
                    event->paint.repaint=1;
                } else if (event->key.code == K_DOWN) {
                    menuPanel->selected++;
                    event->paint.repaint=1;
                } else if (event->key.c == '@') {
                    menuPanel->switchTo = menuItemDesc[menuPanel->selected].panel;
                } else if (event->key.c == '!') {
                    menuPanel->switchTo = DIAL_PANEL;
                }
                {
                    int n = sizeof(menuItemDesc)/sizeof(*menuItemDesc);
                    menuPanel->selected = (menuPanel->selected + n) % n;
                }
            }
                 } break;
                 default:break;
    }
}


struct EnumDesc callStateDesc[] = {
    {GSTATE_CALL_OUT_CONNECTED, "answered"},
    {GSTATE_CALL_IN_CONNECTED, "answered"},
    {GSTATE_CALL_IN_INVITE, "[ "},
    {GSTATE_CALL_OUT_INVITE, "calling"},
    {0,0}
};

void storeCalledNumber(struct HistoryItem *item) {
    FILE *f=fopen(HISTORY_FILE, "ab");
    assert(f);
    fwrite(item, sizeof(*item), 1, f);
    fclose(f);
}

void beepSpeaker(float freq) {
    fprintf(stderr, "beepSpeaker %i\n", freq); 
    if (freq) {
           /* BEEP_TYPE_EVDEV */
           struct input_event e;
      
           e.type = EV_SND;
           e.code = SND_TONE;
           e.value = freq;
      
           write(console_fd, &e, sizeof(struct input_event));

    } else {
           /* BEEP_TYPE_EVDEV */
           struct input_event e;
      
           e.type = EV_SND;
           e.code = SND_TONE;
           e.value = 0;
      
           write(console_fd, &e, sizeof(struct input_event));
    }
}

void ring(int v) {
    if (v) {
        set_yldisp_ringer(YL_RINGER_ON,1);
    } else {
        set_yldisp_ringer(YL_RINGER_OFF,0);
    }
}

int findIncomingKnown(char *incomingNumber, struct Dictionary *dictionary) {
    int i;
    for (i=0;i<DICTIONARY_SIZE;i++) {
        if (strcmp(incomingNumber, dictionary->items[i].number) == 0) return i;
    }
    return -1;
}

void callPanelEvent(struct Event *event, struct CallPanel *callPanel) {
    switch (event->type) {
        case INIT: {
            callPanel->callState = -1;
            callPanel->callTimer = 0;
            callPanel->connected = 0;
            callPanel->incomingKnown = -1;
            melodyStop(&callPanel->melodyPlayer);
        } break;
        case PAINT: {
            const char *desc = getEnumDesc(callPanel->callState, callStateDesc);
            snprintf(event->paint.display->text, 13, "%s%s", desc, callPanel->incomingKnown == -1 ? callPanel->incomingNumber : callPanel->dictionary->items[callPanel->incomingKnown].name);
        } break;
        case KEY: {
            if (event->key.value) {
                if (event->key.c == '!') {
                    lpstates_submit_command(LPCOMMAND_HANGUP, NULL);
                } else if (callPanel->callState ==  GSTATE_CALL_IN_INVITE && event->key.c == '@') {
                    lpstates_submit_command(LPCOMMAND_PICKUP, NULL);
                }
            }
        } break;
        case TIMER: {
            if (checkTimePassed(&callPanel->callTimer, event)) {
                if (callPanel->connected == 0) {
                    struct Melody* melody;
                    if (callPanel->incomingKnown == -1) {
                        melody = &callPanel->dictionary->defaultMelody;
                    } else {
                        melody = &callPanel->dictionary->items[callPanel->incomingKnown].melody;
                    }
                    if (melody->type == MELODY_DEFAULT) {
                        ring(1);
                    } else {
                        melodyStart(&callPanel->melodyPlayer, melody);
                    }
                }
            }
            melodyTimerEvent(&callPanel->melodyPlayer, event);
        } break;
        default:break;
    }
    if (event->type == VOIP) {
        callPanel->callState = event->voip.call;
        event->paint.repaint = 1;
        switch (event->voip.call) {
            case GSTATE_CALL_IN_CONNECTED: 
                ring(0);
                melodyStop(&callPanel->melodyPlayer);
                callPanel->connected = 1;
            case GSTATE_CALL_OUT_CONNECTED: {
                yldisp_start_counter();
                callPanel->callInfo.callStarted = now();
                callPanel->connected = 1;
            } break;
            case GSTATE_CALL_IN_INVITE: {
                extract_callernum(callPanel->incomingNumber, event->voip.message);
                callPanel->incomingKnown = findIncomingKnown(callPanel->incomingNumber, callPanel->dictionary);
                strcpy(callPanel->callInfo.number, callPanel->incomingNumber);
                callPanel->callInfo.isOutgoing = 0;
                initTimerAfter(170000,&callPanel->callTimer);
            } break;
            case GSTATE_CALL_OUT_INVITE: {
                callPanel->callInfo.isOutgoing = 1;
                callPanel->incomingNumber[0]=0;
            } break;
            
            case GSTATE_CALL_IDLE:
            case GSTATE_CALL_END: 
            case GSTATE_CALL_ERROR: {
                ring(0);
                melodyStop(&callPanel->melodyPlayer);

                callPanel->callInfo.callEnded = now();
                storeCalledNumber(&callPanel->callInfo);
                callPanel->incomingNumber[0]=0;
            }

            default:break;
        }
    }

}

void mainPanelChildEvent(struct Event *event, struct MainPanel *mainPanel) {
    switch (mainPanel->enabled) {
        case DIAL_PANEL: dialPanelEvent(event, &mainPanel->dialPanel); break;
        case MENU_PANEL: menuPanelEvent(event, &mainPanel->menuPanel); break;
        case CALL_PANEL: callPanelEvent(event, &mainPanel->callPanel); break;
        case SEARCH_PANEL: searchPanelEvent(event, &mainPanel->searchPanel); break;
        case HISTORY_PANEL: historyPanelEvent(event, &mainPanel->historyPanel); break;
        case VOIP_STATUS_PANEL: voipStatusPanelEvent(event, &mainPanel->voipStatusPanel); break;
        case NONE: assert(0);
    }
}

void mainPanelSwitchTo(enum MainPanelItem *switchTo, struct Event *event, struct MainPanel *mainPanel) {
    struct Event event2;
    if (*switchTo != NONE) {
        event2.type = INIT;
        mainPanel->enabled = *switchTo;
        mainPanelChildEvent(&event2, mainPanel);
        *switchTo = NONE;
        event->paint.repaint = 1;
    }
}

void mainPanelEvent(struct Event *event, struct MainPanel *mainPanel) {
    if (event->type == INIT) {
        mainPanel->enabled = DIAL_PANEL;
        mainPanel->searchPanel.dialPanel = &mainPanel->dialPanel;
        mainPanel->historyPanel.dialPanel = &mainPanel->dialPanel;
        mainPanel->dialPanel.callPanel = &mainPanel->callPanel;
        loadDictionary(&mainPanel->searchPanel.dictionary);
        mainPanel->callPanel.dictionary = &mainPanel->searchPanel.dictionary;
    }
    if (event->type == VOIP) {
        //fprintf(stderr,"VOIP\n");
        mainPanel->voipStatusPanel.voip = event->voip;
        switch (event->voip.call) {
            case GSTATE_CALL_OUT_CONNECTED:
            case GSTATE_CALL_IN_CONNECTED:
            case GSTATE_CALL_IN_INVITE:
            case GSTATE_CALL_OUT_INVITE: 
            if (mainPanel->enabled != CALL_PANEL) {
                enum MainPanelItem switchTo = CALL_PANEL;
                mainPanelSwitchTo(&switchTo, event, mainPanel);
            }
            break;

            case GSTATE_CALL_IDLE:
            case GSTATE_CALL_END: 
            case GSTATE_CALL_ERROR: 
            if (mainPanel->enabled == CALL_PANEL) {
                enum MainPanelItem switchTo = DIAL_PANEL;
                mainPanelChildEvent(event, mainPanel);
                mainPanelSwitchTo(&switchTo, event, mainPanel);
            }
            default:break;
        }
    }
    mainPanelChildEvent(event, mainPanel);
    switch (mainPanel->enabled) {
        case DIAL_PANEL: mainPanelSwitchTo(&mainPanel->dialPanel.switchTo, event, mainPanel); break;
        case MENU_PANEL: mainPanelSwitchTo(&mainPanel->menuPanel.switchTo, event, mainPanel); break;
        case SEARCH_PANEL: mainPanelSwitchTo(&mainPanel->searchPanel.switchTo, event, mainPanel); break;
        case HISTORY_PANEL: mainPanelSwitchTo(&mainPanel->historyPanel.switchTo, event, mainPanel); break;
        case VOIP_STATUS_PANEL: mainPanelSwitchTo(&mainPanel->voipStatusPanel.switchTo, event, mainPanel); break;
        case EXIT_PANEL: exit(0); break;

        case NONE:
        case CALL_PANEL:
        break;
    }
}

void mainPanelPaintToDevice(struct MainPanel *mainPanel) {
    struct Event event;
    struct Display display;
    event.type=PAINT;
    event.paint.display = &display;
    mainPanelEvent(&event, mainPanel);
    char buf[13];
    strcpy(buf, "            ");
    strncpy(buf, display.text, strlen(display.text));
    fprintf(stderr,"PAINT:'%s'\n",buf);
    set_yldisp_text(buf);
}

void mainPanelEventWithRepaint(struct Event *event, struct MainPanel *mainPanel) {
    event->paint.repaint = 0;
    mainPanelEvent(event,mainPanel);
    if (event->paint.repaint) {
        event->paint.repaint = 0;
        mainPanelPaintToDevice(mainPanel);
    }
}

/*****************************************************************/

//void display_dialnum(char *num) {
//    int len = strlen(num);
//    if (len < 12) {
//        char buf[13];
//        strcpy(buf, "                        ");
//        strncpy(buf, num, len);
//        set_yldisp_text(buf);
//    }
//    else {
//        set_yldisp_text(num + len - 12);
//    }
//}

/**********************************/

void extract_callernum(char *incomingNumber, const char *line) {
    int err;
    char *line1 = NULL;
    osip_from_t *url;
    char *num;
    char *ptr;
    int what;
    
    incomingNumber[0] = '\0';
    
    if (line && line[0]) {
        osip_from_init(&url);
        err = osip_from_parse(url, line);
        what = (err < 0) ? 2 : 0;
        
        while ((what < 3) && !incomingNumber[0]) {
            if (what == 2)
                line1 = strdup(line);
            
            num = (what == 0) ? url->url->username : 
                        (what == 1) ? url->displayname : line1;
            what++;
            
            if (num && num[0]) {
                /*printf("trying %s\n", num);*/
                
                /* remove surrounding quotes */
                if (num[0] == '"' && num[strlen(num) - 1] == '"') {
                    num[strlen(num) - 1] = '\0';
                    num++;
                }
                
                /* first check for the country code */
                int intl = 0;
                if (num[0] == '+') {
                    /* assume "+<country-code><area-code><local-number>" */
                    intl = 1;
                    num++;
                }
                else
                /* check if 'num' consists of numbers only */
                ptr = num;
                while (ptr && *ptr) {
                    if (*ptr > '9' || *ptr < '0')
                        ptr = NULL;
                    else
                        ptr++;
                }
                if (!ptr || !*num) {
                    /* we found other characters -> skip this string */
                    continue;
                }

                strncat(incomingNumber, num, MAX_NUMBER_LEN-1);
            }
        }
        osip_from_free(url);
        if (line1)
         free(line1);
    }
}

/**********************************/

//void handle_key(ylcontrol_data_t *ylc_ptr, int code, int value) {
//    char c;
//    gstate_t lpstate_power;
//    gstate_t lpstate_call;
//    gstate_t lpstate_reg;
//    
//    lpstate_power = gstate_get_state(GSTATE_GROUP_POWER);
//    lpstate_call = gstate_get_state(GSTATE_GROUP_CALL);
//    lpstate_reg = gstate_get_state(GSTATE_GROUP_REG);
//    //dump_lpstate();
//    
//    if (code == 42) {         /* left shift */
//        ylc_ptr->kshift = value;
//        ylc_ptr->pressed = -1;
//    }
//    else {
//        ylc_ptr->pressed = (value) ? code : -1;
//        if (value) {
//            /*printf("key=%d\n", code);*/
//            switch (code) {
//                case 2:             /* '1'..'9' */
//                case 3:
//                case 4:
//                case 5:
//                case 6:
//                case 7:
//                case 8:
//                case 9:
//                case 10:
//                case 11:            /* '0' */
//                case 55:            /* '*' */
//                    if (lpstate_power != GSTATE_POWER_ON)
//                        break;
//                    /* get the real character */
//                    c = (code == 55) ? '*' :
//                            (code == 4 && ylc_ptr->kshift) ? '#' :
//                            (code == 11) ? '0' : ('0' + code - 1);
//
//                    fprintf(stderr,"key=%c\n", c);
//
//                    if (lpstate_call == GSTATE_CALL_IDLE &&
//                            lpstate_reg    == GSTATE_REG_OK) {
//                        int len = strlen(ylc_ptr->dialnum);
//                        
//                        if (c == '#') {
//                            /* the store/recall character */
//                            if ((len > 0) || ylc_ptr->dialback[0]) {
//                                /* prepare to store the currently displayed number */
//                                ylc_ptr->prep_store = 1;
//                                set_yldisp_store_type(YL_STORE_ON);
//                            }
//                            else {
//                                /* prepare to recall a number */
//                                ylc_ptr->prep_recall = 1;
//                                set_yldisp_text("    select        ");
//                            }
//                        }
//                        else
//                        if (c >= '0' && c <= '9') {
//                            if (ylc_ptr->prep_store) {
//                                /* store number */
//                                char *key;
//                                key = strdup("mem ");
//                                key[3] = c;
//                                ypconfig_set_pair(key, (len) ? ylc_ptr->dialnum : ylc_ptr->dialback);
//                                free(key);
//                                ypconfig_write(NULL);
//                                ylc_ptr->prep_store = 0;
//                                set_yldisp_store_type(YL_STORE_NONE);
//                            }
//                            else
//                            if (ylc_ptr->prep_recall) {
//                                /* recall number but do not dial yet */
//                                char *key;
//                                char *val;
//                                key = strdup("mem ");
//                                key[3] = c;
//                                val = ypconfig_get_value(key);
//                                if (val && *val) {
//                                    strncpy(ylc_ptr->dialback, val, MAX_NUMBER_LEN);
//                                }
//                                free(key);
//                                ylc_ptr->prep_recall = 0;
//                                display_dialnum(ylc_ptr->dialback);
//                            }
//                            else {
//                                /* we want to dial for an outgoing call */
//                                if (len + 1 < sizeof(ylc_ptr->dialnum)) {
//                                    ylc_ptr->dialnum[len + 1] = '\0';
//                                    ylc_ptr->dialnum[len] = c;
//                                    display_dialnum(ylc_ptr->dialnum);
//                                }
//                                ylc_ptr->dialback[0] = '\0';
//                            }
//                        }
//                        else {
//                            /* do not handle '*' for now ... */
//                        }
//                    }
//                    else
//                    if (lpstate_call == GSTATE_CALL_OUT_CONNECTED ||
//                            lpstate_call == GSTATE_CALL_IN_CONNECTED) {
//                        char buf[2];
//                        buf[0] = c;
//                        buf[1] = '\0';
//                        lpstates_submit_command(LPCOMMAND_DTMF, buf);
//                    }
//                    else
//                    if (lpstate_call == GSTATE_CALL_IN_INVITE &&
//                            c == '#') {
//                        set_yldisp_ringer(YL_RINGER_OFF);
//                    }
//                    break;
//
//                case 14:                 /* C */
//                    if (lpstate_power != GSTATE_POWER_ON)
//                        break;
//                    if (lpstate_call == GSTATE_CALL_IDLE &&
//                            lpstate_reg    == GSTATE_REG_OK) {
//                        int len = strlen(ylc_ptr->dialnum);
//                        if (ylc_ptr->prep_store) {
//                            ylc_ptr->prep_store = 0;
//                            set_yldisp_store_type(YL_STORE_NONE);
//                        }
//                        else {
//                            if (len > 0) {
//                                ylc_ptr->dialnum[len - 1] = '\0';
//                            }
//                            ylc_ptr->dialback[0] = '\0';
//                            ylc_ptr->prep_recall = 0;
//                            display_dialnum(ylc_ptr->dialnum);
//                        }
//                    }
//                    break;
//
//                case 28:                 /* pick up */
//                    if (lpstate_power != GSTATE_POWER_ON)
//                        break;
//                    if (lpstate_call == GSTATE_CALL_IDLE &&
//                            lpstate_reg    == GSTATE_REG_OK) {
//                        if (strlen(ylc_ptr->dialnum) == 0 &&
//                                strlen(ylc_ptr->dialback) > 0) {
//                            /* dial the current number displayed */
//                            strcpy(ylc_ptr->dialnum, ylc_ptr->dialback);
//                        }
//                        if (strlen(ylc_ptr->dialnum) > 0) {
//                            strcpy(ylc_ptr->dialback, ylc_ptr->dialnum);
//                            lpstates_submit_command(LPCOMMAND_CALL, ylc_ptr->dialnum);
//                            
//                            /* TODO: add number to history */
//                            
//                            ylc_ptr->dialnum[0] = '\0';
//                        }
//                        else {
//                            /* TODO: display history */
//                        }
//                    }
//                    else
//                    if (lpstate_call == GSTATE_CALL_IN_INVITE) {
//                        lpstates_submit_command(LPCOMMAND_PICKUP, NULL);
//                    }
//                    break;
//
//                case 1:                    /* hang up */
//                    if (lpstate_power != GSTATE_POWER_ON)
//                        break;
//                    if (lpstate_call == GSTATE_CALL_OUT_INVITE ||
//                            lpstate_call == GSTATE_CALL_OUT_CONNECTED ||
//                            lpstate_call == GSTATE_CALL_IN_INVITE ||
//                            lpstate_call == GSTATE_CALL_IN_CONNECTED) {
//                        lpstates_submit_command(LPCOMMAND_HANGUP, NULL);
//                    }
//                    else
//                    if (lpstate_call == GSTATE_CALL_IDLE &&
//                            lpstate_reg    == GSTATE_REG_OK) {
//                        ylc_ptr->dialnum[0] = '\0';
//                        ylc_ptr->dialback[0] = '\0';
//                        ylc_ptr->prep_store = 0;
//                        ylc_ptr->prep_recall = 0;
//                        set_yldisp_store_type(YL_STORE_NONE);
//                        display_dialnum("");
//                    }
//                    break;
//                
//                case 105:                /* VOL- */
//                    if (lpstate_call == GSTATE_CALL_OUT_CONNECTED ||
//                            lpstate_call == GSTATE_CALL_IN_CONNECTED) {
//                        lpstates_submit_command(LPCOMMAND_SPKR_VOLDN, NULL);
//                    }
//                    else
//                    if (lpstate_call == GSTATE_CALL_IN_INVITE /*||
//                            lpstate_call == GSTATE_CALL_IDLE*/) {
//                        lpstates_submit_command(LPCOMMAND_RING_VOLDN, NULL);
//                    }
//                    break;
//                
//                case 106:                /* VOL+ */
//                    if (lpstate_call == GSTATE_CALL_OUT_CONNECTED ||
//                            lpstate_call == GSTATE_CALL_IN_CONNECTED) {
//                        lpstates_submit_command(LPCOMMAND_SPKR_VOLUP, NULL);
//                    }
//                    else
//                    if (lpstate_call == GSTATE_CALL_IN_INVITE /*||
//                            lpstate_call == GSTATE_CALL_IDLE*/) {
//                        lpstates_submit_command(LPCOMMAND_RING_VOLUP, NULL);
//                    }
//                    break;
//                
//                case 103:                /* UP */
//                    break;
//                
//                case 108:                /* DOWN */
//                    break;
//                
//                default:
//                    break;
//            }
//        }
//    }
//}
//
///**********************************/
//
//void handle_long_key(ylcontrol_data_t *ylc_ptr, int code) {
//    gstate_t lpstate_power;
//    gstate_t lpstate_call;
//    
//    lpstate_power = gstate_get_state(GSTATE_GROUP_POWER);
//    lpstate_call = gstate_get_state(GSTATE_GROUP_CALL);
//    
//    switch (code) {
//        case 14:                 /* C */
//            if (lpstate_power != GSTATE_POWER_ON)
//                break;
//            if (lpstate_call == GSTATE_CALL_IDLE) {
//                ylcontrol_data.dialnum[0] = '\0';
//                display_dialnum("");
//            }
//            break;
//        
//        case 1:                    /* hang up */
//            if (lpstate_power == GSTATE_POWER_OFF) {
//                lpstates_submit_command(LPCOMMAND_STARTUP, NULL);
//            }
//            else
//            if (lpstate_power != GSTATE_POWER_OFF &&
//                    lpstate_power != GSTATE_POWER_SHUTDOWN) {
//                lpstates_submit_command(LPCOMMAND_SHUTDOWN, NULL);
//            }
//            break;
//        
//        default:
//            break;
//    }
//}

/**********************************/

#define STRING2(x) #x
#define STRING(x) STRING2(x)

void dump_lpstate() {
    gstate_t lpstate_power;
    gstate_t lpstate_call;
    gstate_t lpstate_reg;
    
    lpstate_power = gstate_get_state(GSTATE_GROUP_POWER);
    lpstate_call = gstate_get_state(GSTATE_GROUP_CALL);
    lpstate_reg = gstate_get_state(GSTATE_GROUP_REG);

#define DO(s,x) if (x == s) { fprintf(stderr, STRING(x) "=" STRING(s) "        "); }

#define DO_ALL(z) \
DO(GSTATE_CALL_END,z)\
DO(GSTATE_CALL_ERROR,z)\
DO(GSTATE_CALL_IDLE,z)\
DO(GSTATE_CALL_IN_CONNECTED,z)\
DO(GSTATE_CALL_IN_INVITE,z)\
DO(GSTATE_CALL_OUT_CONNECTED,z)\
DO(GSTATE_CALL_OUT_INVITE,z)\
DO(GSTATE_POWER_OFF,z)\
DO(GSTATE_POWER_ON,z)\
DO(GSTATE_POWER_SHUTDOWN,z)\
DO(GSTATE_POWER_STARTUP,z)\
DO(GSTATE_REG_FAILED,z)\
DO(GSTATE_REG_OK,z)\

//DO(GSTATE_GROUP_CALL,z)
//DO(GSTATE_GROUP_POWER,z)
//DO(GSTATE_GROUP_REG,z)
//

    DO_ALL(lpstate_power)
    DO_ALL(lpstate_call)
    DO_ALL(lpstate_reg)

#undef DO
#undef DO_ALL

    fprintf(stderr,"\n");
}



struct MainPanel *mainPanel;

pthread_mutex_t panelMutex;

void panelLock() {
    fprintf(stderr,"panelLock\n");
    pthread_mutex_lock(&panelMutex);
}

void panelUnlock() {
    fprintf(stderr,"panelUnlock\n");
    pthread_mutex_unlock(&panelMutex);
}


void lps_callback(struct _LinphoneCore *lc,
                                    LinphoneGeneralState *gstate) {
    gstate_t lpstate_power;
    gstate_t lpstate_call;
    gstate_t lpstate_reg;
    
    fprintf(stderr,"message: %s\n", gstate->message);
    dump_lpstate();

    lpstate_power = gstate_get_state(GSTATE_GROUP_POWER);
    lpstate_call = gstate_get_state(GSTATE_GROUP_CALL);
    lpstate_reg = gstate_get_state(GSTATE_GROUP_REG);

    {
        struct PipeEvent event;
        event.type = VOIP_PIPE;
        event.voip.power = lpstate_power;
        event.voip.call = lpstate_call;
        event.voip.reg = lpstate_reg;
        event.voip.message[0]=0;
        if (gstate->message)
            strcpy(event.voip.message, gstate->message);
        sendPipeEvent(&event);
    }
    
//    switch (gstate->new_state) {
//        case GSTATE_POWER_OFF:
//            yldisp_led_off();
//            yldisp_hide_all();
//            set_yldisp_text("     - off -    ");
//            break;
//            
//        case GSTATE_POWER_STARTUP:
//            yldisp_show_date();
//            set_yldisp_text("- startup - ");
//            yldisp_led_blink(150, 150);
//            break;
//            
//        case GSTATE_POWER_ON:
//            set_yldisp_text("- register -");
//            break;
//            
//        case GSTATE_REG_FAILED:
//            if (lpstate_power != GSTATE_POWER_ON)
//                break;
//            if (lpstate_call == GSTATE_CALL_IDLE) {
//                set_yldisp_text("-reg failed1-");
//                ylcontrol_data.dialback[0] = '\0';
//                ylcontrol_data.dialnum[0] = '\0';
//                yldisp_led_blink(150, 150);
//            }
//            break;
//            
//        case GSTATE_POWER_SHUTDOWN:
//            yldisp_led_blink(150, 150);
//            yldisp_hide_all();
//            set_yldisp_text("- shutdown -");
//            break;
//            
//        case GSTATE_REG_OK:
//            if (lpstate_power != GSTATE_POWER_ON)
//                break;
//            if (lpstate_call == GSTATE_CALL_IDLE) {
//                display_dialnum("");
//                ylcontrol_data.dialback[0] = '\0';
//                ylcontrol_data.dialnum[0] = '\0';
//                yldisp_led_on();
//            }
//            break;
//            
//        case GSTATE_CALL_IDLE:
//            fprintf(stderr,"GSTATE_CALL_IDLE\n");
//            if (lpstate_power != GSTATE_POWER_ON)
//                break;
//
//            if (lpstate_reg == GSTATE_REG_FAILED) {
//                set_yldisp_text("-reg failed2-");
//                ylcontrol_data.dialback[0] = '\0';
//                ylcontrol_data.dialnum[0] = '\0';
//                yldisp_led_blink(150, 150);
//            }
//            else if (lpstate_reg == GSTATE_REG_OK) {
//                yldisp_led_on();
//            }
//            break;
//            
//        case GSTATE_CALL_IN_INVITE:
//            extract_callernum(&ylcontrol_data, gstate->message);
//            if (strlen(ylcontrol_data.callernum))
//                display_dialnum(ylcontrol_data.callernum);
//            else
//                display_dialnum(" - - -");
//            
//            set_yldisp_call_type(YL_CALL_IN);
//            yldisp_led_blink(300, 300);
//            
//            /* ringing seems to block displaying line 3,
//             * so we have to wait for about 170ms.
//             * This seems to be a limitation of the hardware */
//            usleep(170000);
//            set_yldisp_ringer(YL_RINGER_ON);
//            
//            strcpy(ylcontrol_data.dialback, ylcontrol_data.callernum);
//            ylcontrol_data.dialnum[0] = '\0';
//            break;
//            
//        case GSTATE_CALL_IN_CONNECTED:
//            set_yldisp_ringer(YL_RINGER_OFF);
//            /* start timer */
//            yldisp_start_counter();
//            yldisp_led_blink(1000, 100);
//            /*yldisp_led_on();*/
//            break;
//            
//        case GSTATE_CALL_OUT_INVITE:
//            set_yldisp_call_type(YL_CALL_OUT);
//            yldisp_led_blink(300, 300);
//            break;
//            
//        case GSTATE_CALL_OUT_CONNECTED:
//            /* start timer */
//            yldisp_start_counter();
//            yldisp_led_blink(1000, 100);
//            /*yldisp_led_on();*/
//            break;
//            
//        case GSTATE_CALL_END:
//            set_yldisp_ringer(YL_RINGER_OFF);
//            set_yldisp_call_type(YL_CALL_NONE);
//            yldisp_show_date();
//            yldisp_led_on();
//            break;
//            
//        case GSTATE_CALL_ERROR:
//            ylcontrol_data.dialback[0] = '\0';
//            set_yldisp_call_type(YL_CALL_NONE);
//            set_yldisp_text(" - error -    ");
//            yldisp_show_date();
//            yldisp_led_on();
//            break;
//            
//        default:
//            break;
//    }
}

/**********************************/

void handle_key(int code, int value) {
    struct Event event;
    panelLock();
    event.type = KEY;
                event.key.c = 
        (code == 55) ? '*' :
                (code == 14) ? '^' :
                (code == 1) ? '!' :
                (code == 28) ? '@' :
                            (code == 11) ? '0' : ('0' + code - 1);
    event.key.code = code;
    event.key.value = value;
    event.paint.repaint = 0;
    mainPanelEventWithRepaint(&event, mainPanel);
    panelUnlock();
}

long long handle_timer() {
                struct Event event;
                long long timeout;
                panelLock();
                event.type = TIMER;
                event.timer.smallestNext = now() + 30 * 1000000;
                mainPanelEventWithRepaint(&event, mainPanel);
                timeout = event.timer.smallestNext;
                panelUnlock();
                return timeout;
}

void *control_proc(void *arg) {
    ylcontrol_data_t *ylc_ptr = arg;
    int bytes;
    struct input_event event;
    fd_set master_set, read_set;
    int max_fd;
    long long timeout = 10 * 1000000;
    
    FD_ZERO(&master_set);
    FD_SET(ylc_ptr->evfd, &master_set);
    FD_SET(eventPipe[0], &master_set);
    max_fd = ylc_ptr->evfd + 1;
    
    ylc_ptr->kshift = 0;
    ylc_ptr->dialnum[0] = '\0';
    ylc_ptr->dialback[0] = '\0';
    ylc_ptr->prep_store = 0;
    ylc_ptr->prep_recall = 0;

    while (1) {
        int retval;
        
        memcpy(&read_set, &master_set, sizeof(master_set));
        
        {
            struct timeval timeout3;
            long long timeout2,_now=now();
            while (timeout < _now) {
        timeout = handle_timer();
            }
            timeout2 = timeout - _now;
            if (timeout2 < 1000) timeout2 = 1000;
            llToTimeval(timeout2, &timeout3);
            //retval = select(max_fd, &read_set, NULL, NULL/*&excpt_set*/, &timeout);
            retval = select(max_fd, &read_set, NULL, NULL, &timeout3);
        }
        
        if (retval > 0) {        /* we got an event */
            if (FD_ISSET(ylc_ptr->evfd, &read_set)) {
            
                bytes = read(ylc_ptr->evfd, &event, sizeof(struct input_event));
                
                if (bytes != (int) sizeof(struct input_event)) {
                    fprintf(stderr, "control_proc: Expected %d bytes, got %d bytes\n",
                                    sizeof(struct input_event), bytes);
                    abort();
                }
                
                if (event.type == 1) {                /* key */
                    //handle_key(&ylcontrol_data, event.code, event.value);
                    handle_key(event.code, event.value);
                }
            }

            if (FD_ISSET(eventPipe[0], &read_set)) {
                struct PipeEvent e;
                fprintf(stderr,"read eventPipe\n");
                assert(read(eventPipe[0], &e, sizeof(e)) == sizeof(e));
                switch (e.type) {
                    case TAP_TIMER: {
                        timeout = handle_timer();
                        fprintf(stderr,"TAP_TIMER timeout=%lld\n", timeout);
                    } break;
                    case VOIP_PIPE: {
                        struct Event event;
                        panelLock();
                        event.type = VOIP;
                        event.voip = e.voip;
                        mainPanelEventWithRepaint(&event, mainPanel);
                        panelUnlock();
                    }
                }
            }
        } else if (retval == 0) {     /* timeout */
        } else {
            /* select error */
            assert(0);
        }
    }
}

/*****************************************************************/

void init_ylcontrol(char *countrycode) {
    int modified = 0;
    
    assert(pipe(eventPipe)==0);
    fprintf(stderr,"init eventPipe read:%i write:%i\n",eventPipe[0], eventPipe[1]);
    pthread_mutex_init(&panelMutex, NULL);

    console_fd = open("/dev/input/event0", O_WRONLY);
    
    {
        struct Event event;
        panelLock();
        event.type=INIT;
        mainPanel = malloc(sizeof(*mainPanel));
        memset(mainPanel, 0, sizeof(*mainPanel));
        assert(mainPanel != 0);
        mainPanelEvent(&event,mainPanel);
        mainPanelPaintToDevice(mainPanel);
        panelUnlock();
    }
    set_lpstates_callback(lps_callback);
    
    ylcontrol_data.intl_access_code = ypconfig_get_value("intl-access-code");
    if (!ylcontrol_data.intl_access_code) {
        ylcontrol_data.intl_access_code = "00";
        ypconfig_set_pair("intl-access-code", ylcontrol_data.intl_access_code);
        modified = 1;
    }
    ylcontrol_data.natl_access_code = ypconfig_get_value("natl-access-code");
    if (!ylcontrol_data.natl_access_code) {
        ylcontrol_data.natl_access_code = "0";
        ypconfig_set_pair("natl-access-code", ylcontrol_data.natl_access_code);
        modified = 1;
    }
    ylcontrol_data.country_code = ypconfig_get_value("country-code");
    if (!ylcontrol_data.country_code) {
        ylcontrol_data.country_code = "";
        ypconfig_set_pair("country-code", ylcontrol_data.country_code);
        modified = 1;
    }
    if (modified) {
        /* write back modified configuration */
        ypconfig_write(NULL);
    }
}

/*************************************/

void start_ylcontrol() {
    char *path_event;
    
    path_event = get_yldisp_event_path();
    
    ylcontrol_data.evfd = open(path_event, O_RDONLY);
    if (ylcontrol_data.evfd < 0) {
        perror(path_event);
        abort();
    }
    
    /* grab the event device to prevent it from propagating
         its events to the regular keyboard driver                        */
    if (ioctl(ylcontrol_data.evfd, EVIOCGRAB, (void *)1)) {
        perror("EVIOCGRAB");
        abort();
    }
    
    pthread_create(&(ylcontrol_data.control_thread), NULL, control_proc, &ylcontrol_data);
}

void stop_ylcontrol() {
}

//------------------------------------
