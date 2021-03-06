//class Model//
#include "Point.h"
#include "Step.h"
#include "SpeedManager.h"

#ifndef MODEL_H
#define MODEL_H

SpeedManager *sm;
Point *Model_toPoint;

typedef enum solenoidState{
    solenoidStateUp = 0,
    solenoidStateDown = 1
}SolenoidState;

typedef enum penMode{  
	penModeImage = 1,
	penModeManualUp = 2,
	penModeManualDown = 3
}PenMode;

typedef struct FSBotState{
	int retainCount;
	char *type;
	Point *home;
	Point *currentLocation;
	SpeedManager *speedManager;
	int leftsteps;
	int rightsteps;
	int delay;
	void (*executeStepCallback)(Step *step);
	PenMode penMode;
	PenMode scheduledPenMode;
}BotState;

BotState *BOT;

void SpeedManager_callback(float x, float y, int delay, int cursor,int penMode);
void Model_createInstance();
void Model_addStep(int left, int right);
void Model_logState();
void Model_release();
void Model_retain();
void Model_moveTo(Point *dest);
void Model_moveHome();
void Model_setExecuteStepCallback(void (*executeStepCallback)(Step *step));
void Model_setPenMode(PenMode mode);
void Model_finish();
void Model_resume();

void report_memory(int id);

#endif


