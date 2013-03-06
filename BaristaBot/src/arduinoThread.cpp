
#include "ofMain.h"
#include "arduinoThread.h"
#include "motorThread.h"


//----------------------------------------------------------------------------------------------
void arduinoThread::start(){
    startThread(true, false);   // blocking, not-verbose
    curState = START;
}

void arduinoThread::stop(){
    stopThread();
    ard.disconnect();
}

//----------------------------------------------------------------------------------------------
void arduinoThread::setup(){
    // do not change this sequence
    initializeVariables();
    initializeArduino();
}

void arduinoThread::initializeVariables(){
    start_path = false;
    start_transition = true;
    home = ofPoint(HOME_X, HOME_Y);
    
    x_timer = y_timer = i_timer = ofGetElapsedTimeMicros();
    x_steps = y_steps = i_steps = x_inc = y_inc = i_inc = 0;
    x_go = y_go = i_go = ARD_LOW;
}


void arduinoThread::initializeArduino() {
    // StandardFirmata for OF is at 57600 by default
	ard.connect("/dev/tty.usbmodem1411", 115200);
	ofAddListener(ard.EInitialized, this, &arduinoThread::setupArduino);
	bSetupArduino = false;
}

void arduinoThread::setupArduino(const int & version) {
    // remove listener because we don't need it anymore
    ofRemoveListener(ard.EInitialized, this, &arduinoThread::setupArduino);
    
    // set digital outputs
    ard.sendDigitalPinMode(X_DIR_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(Z_DIR_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(Y_DIR_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(INK_DIR_PIN, ARD_OUTPUT);
    
    ard.sendDigitalPinMode(X_STEP_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(Z_STEP_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(Y_STEP_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(INK_STEP_PIN, ARD_OUTPUT);
    
    ard.sendDigitalPinMode(X_SLEEP_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(Z_SLEEP_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(Y_SLEEP_PIN, ARD_OUTPUT);
    ard.sendDigitalPinMode(INK_SLEEP_PIN, ARD_OUTPUT);
    
    // set digital inputs
    ard.sendDigitalPinMode(X_LIMIT_PIN, ARD_INPUT);
    ard.sendDigitalPinMode(Z_LIMIT_PIN, ARD_INPUT);
    ard.sendDigitalPinMode(Y_LIMIT_PIN, ARD_INPUT);
    ard.sendDigitalPinMode(INK_LIMIT_PIN, ARD_INPUT);
    ofAddListener(ard.EDigitalPinChanged, this, &arduinoThread::digitalPinChanged);

    // it is now safe to send commands to the Arduino
    bSetupArduino = true;
    curState = IDLE;
}









//----------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
void arduinoThread::update(){
    ard.update();
    
    if (x_inc >= x_steps || x_steps == 0) {
        ard.sendDigital(X_SLEEP_PIN, ARD_LOW);
    }
    if (y_inc >= y_steps || y_steps == 0) {
        ard.sendDigital(Y_SLEEP_PIN, ARD_LOW);
    }
    
    switch (curState) {
        // arm has raised and is ready to take a photo
        case SHOOT_FACE:
            break;
        // photo taken, arm is going to the limit switches: home
        case HOMING:
            break;
        // X, Z, and Y have hit limits
        case HOME:
            journeyOn(true);
            curState = PRINTING;
            break;
        // print has started from first point
        case PRINTING:
            if (journeysDone()) {
                journeyOn(false);
            }
            break;
        // print is finished and arm is raising up
        case DONE:
            break;
        default:
            break;
    }
}


void arduinoThread::journeyOn(bool new_coffee){
    point_count = 1;
    
    if (new_coffee) {
        start_transition = true;
        current = ofPoint(cropped_size, cropped_size);
        target = *points.begin();
        paths_i = points_i = 0;
        fireEngines();
    }
    else {
        planJourney();
        
        // starting a transition
        if (start_transition){
            start_transition = false;
            stopInk();
            fireEngines();
        }
        // starting a new poy, ink flows
        else if (start_path){
            start_path = false;
            startInk();
            fireEngines();
        }
        // drawing last segment in a polyline
        else if (end_path) {
            end_path = false;
            fireEngines();
        }
        // check to see if movements are long enough, if not add a point
        else {
            int sx = abs(getSteps(current.x, target.x, true));
            int sy = abs(getSteps(current.y, target.y, false));
            
            // if we're already above tolerance, fire
            if (sx > TOL && sy > TOL) {
                fireEngines();
            }
            // if one dimension gets too big, fire
            else if (sx > TOL*4 || sy > TOL*4) {
                fireEngines();
            }
            else {
                // if you didn't fire and return out then you're hitting planJourney() again
                point_count++;
            }
        }
    }
}

// increments through path and sets the target point, the next x and y values to move to
void arduinoThread::planJourney(){

    // if the current polyline has just one point, it's a transition
    if (points.size() == 1) {
        // if it's the last polyline just skip it
        if (paths_i == paths.size()-1) {
            shootCoffee();
        }
        // if it's not the last line move to the point and get the next polyline
        else {
            start_transition = true;
            paths_i++;
            points = paths.at(paths_i).getVertices();
            points_i = 0;
            target = points.at(points_i);
        }
    }
    // starting a new path, points_i is where we are currently, where the machine is
    else if (points_i == 0) {
        start_path = true;
        start_transition = false;
        points_i++;
        target = points.at(points_i);
    }
    // continuing a path except for the last stage
    else if (points_i < points.size()-2) {
        points_i++;
        target = points.at(points_i);
    }
    // ending a path
    else if (points_i < points.size()-1) {
        end_path = true;
        points_i++;
        target = points.at(points_i);
    }
    // starting a transition
    else if (paths_i < paths.size()-1) {
        start_transition = true;
        paths_i++;
        points_i = 0;
        points = paths.at(paths_i).getVertices();
        target = points.at(points_i);
    }
    // finishing the print
    else {
        stopInk();
        shootCoffee();
    }
}


void arduinoThread::fireEngines(){
    int sdelta_x = 0;
    int sdelta_y = 0;
    int delay_x = DELAY_MIN;
    int delay_y = DELAY_MIN;
    
    // get steps
    int sx = abs(sdelta_x = getSteps(current.x, target.x, true));
    int sy = abs(sdelta_y = getSteps(current.y, target.y, false));
    
    // if nowhere to go, skip it
    if (sx == 0 && sy == 0) {
        return;
    }
    
    // get delays
    if (sy > sx && sx != 0) {
        delay_x = DELAY_MIN * sy / sx;
    } else if (sx > sy && sy != 0) {
        delay_y = DELAY_MIN * sx / sy;
    }
    
    // toggle direction pins and enable motors
    bool DIR = (sdelta_x > 0) ? ARD_LOW : ARD_HIGH;
    ard.sendDigital(X_SLEEP_PIN, ARD_HIGH);
    ard.sendDigital(X_DIR_PIN, DIR);
    DIR = (sdelta_y > 0) ? ARD_LOW : ARD_HIGH;
    ard.sendDigital(Y_SLEEP_PIN, ARD_HIGH);
    ard.sendDigital(Y_DIR_PIN, DIR);
    
    // send variables to motors and start them
    x_steps = sx;
    x_inc = 0;
    x_delay = delay_x;
    y_steps = sy;
    y_inc = 0;
    y_delay = delay_y;

    // debugging
    hex = "\nsdelta_x:   " + ofToString(sdelta_x) + "\ndelay_x:    " + ofToString(delay_x);
    hwy = "\nsdelta_y:   " + ofToString(sdelta_y) + "\ndelay_y:    " + ofToString(delay_y);

    // after the move, we are at the target, our new current position
    current = target;
}


int arduinoThread::getSteps(float here, float there, bool is_x) {
    // normalize: 0,0 is the upper left corner and 1,1 is lower right
    float ndelta = there/cropped_size - here/cropped_size;
    
    // then convert to mm, an 80 mm square
    float mmdelta = ndelta * 80;
    
    // then convert to steps (NOTE reversing X)
    if (is_x) {
        ex = "\nhere.x:     " + ofToString(int(here/cropped_size*80*SCALE_X*16))
           + "\nthere.x:    " + ofToString(int(there/cropped_size*80*SCALE_X*16)) + hex;
        int sdelta = -int(mmdelta * SCALE_X * 16); // 1/16th steps
        return sdelta;
    } else {
        wy = "\nhere.y:     " + ofToString(int(here/cropped_size*80*SCALE_Y*16))
           + "\nthere.y:    " + ofToString(int(there/cropped_size*80*SCALE_Y*16)) + hwy;
        int sdelta = int(mmdelta * SCALE_Y * 16);
        return sdelta;
    }
}


bool arduinoThread::journeysDone(){
    if (x_inc < x_steps || y_inc < y_steps)
        return false;
    return true;
}



void arduinoThread::startInk(){
//    //    if (INK.isThreadRunning()) INK.stop();
//    plungerDown();
//    usleep(INK_TIMEOUT/4);    // wait for ink to stop
//    INK.ready(999999, INK_DELAY);
//    if (!INK.isThreadRunning()) INK.start();
}

void arduinoThread::stopInk(){
//    INK.stop();
//    usleep(10000);    // wait for ink to stop
//    plungerUp();            // pull up to fast stop flow
//    usleep(INK_TIMEOUT/2);    // wait for ink to stop
}





//----------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
void arduinoThread::shootFace(){
    curState = SHOOT_FACE;
//    // change these value depending on observation
//    Z.ready(14000, DELAY_MIN);
//    Z.start();
//    while (Z.isThreadRunning()); // wait before doing Y
//    Y.ready(10000, DELAY_MIN);
//    Y.start();
}

void arduinoThread::shootCoffee(){
    curState = SHOOT_COFFEE;
//    // change these value depending on observation
//    Z.ready(3000, 450);
//    Z.start();
//    while (Z.isThreadRunning()); // wait to complete
//
//    ofSleepMillis(1000);
//    // **** TAKE PHOTO ****
//    // operator pushes button to accept it, 
//    // that sends the machine up, ready for next face photo
//    ofSleepMillis(1000);

    shootFace();
}

void arduinoThread::goHome(){
    curState = HOMING;
    X.ready(100000, DELAY_MIN);
    X.start();
    // others go home after pin change events below
}

void arduinoThread::jogRight() {
    if (X.isThreadRunning()) {
        X.INC = 0;
        return;
    }
    X.ready(1000, DELAY_MIN);
    X.start();
}
void arduinoThread::jogLeft() {
    if (X.isThreadRunning()) {
        X.INC = 0;
        return;
    }
    X.ready(-1000, DELAY_MIN);
    X.start();
}
void arduinoThread::jogForward() {
    if (Y.isThreadRunning()) {
        Y.INC = 0;
        return;
    }
    Y.ready(1000, DELAY_MIN);
    Y.start();
}
void arduinoThread::jogBack() {
    if (Y.isThreadRunning()) {
        Y.INC = 0;
        return;
    }
    Y.ready(-1000, DELAY_MIN);
    Y.start();
}
void arduinoThread::jogUp() {
    if (Z.isThreadRunning()) {
        Z.INC = 0;
        return;
    }
    Z.ready(1000, DELAY_MIN);
    Z.start();
}
void arduinoThread::jogDown() {
    if (Z.isThreadRunning()) {
        Z.INC = 0;
        return;
    }
    Z.ready(-1000, DELAY_MIN);
    Z.start();
}
void arduinoThread::plungerUp() {
    if (INK.isThreadRunning()) {
        INK.INC = 0;
        return;
    }
    INK.ready(-500, 800);
    INK.start();
}
void arduinoThread::plungerDown() {
    if (INK.isThreadRunning()) {
        INK.INC = 0;
        return;
    }
    INK.ready(500, 800);
    INK.start();
}


//----------------------------------------------------------------------------------------------
void arduinoThread::threadedFunction(){
    while(isThreadRunning() != 0){
                
        // X axis
        if (ofGetElapsedTimeMicros() - x_timer > x_delay) {
            if (x_steps > 0 && x_inc/2 < x_steps) {
                ard.sendDigital(X_STEP_PIN, x_go = !x_go);
                x_timer = ofGetElapsedTimeMicros();
                x_inc++;
            }
        } else {
            usleep(1); // don't let it run too fast
        }
        // Y axis
        if (ofGetElapsedTimeMicros() - y_timer > y_delay) {
            if (y_steps > 0 && y_inc/2 < y_steps) {
                ard.sendDigital(Y_STEP_PIN, y_go = !y_go);
                y_timer = ofGetElapsedTimeMicros();
                y_inc++;
            }
        } else {
            usleep(1); // don't let it run too fast
        }
    }
}

void arduinoThread::draw(){
   
    string str = "curState = " + ofToString(stateName[curState]);
    if (!bSetupArduino){
		str += "\narduino not ready...";
	} else {
        str += "\nI need a fucking coffee.";
    }
    ofDrawBitmapString(str, 50, 660);
    
    str = "Path:       " + ofToString(paths_i) + "\n\nPoint:      "
        + ofToString(points_i) + "\npoint_count " + ofToString(point_count);
    ofDrawBitmapString(str, 50, 720);
    str = "/ " + ofToString(paths.size()) + "\n\n/ " + ofToString(points.size());
    ofDrawBitmapString(str, 220, 720);

    ofDrawBitmapString(ex, 50, 780);
    ofDrawBitmapString(wy, 220, 780);
    
    str = ":  Step " + ofToString(x_inc);
    ofDrawBitmapString(str, 50, 1000-X_STEP_PIN*7);
    str = " / " + ofToString(x_steps);
    ofDrawBitmapString(str, 220, 1000-X_STEP_PIN*7);
    
    str = ":  Step " + ofToString(y_inc);
    ofDrawBitmapString(str, 50, 1000-Y_STEP_PIN*7);
    str = " / " + ofToString(y_steps);
    ofDrawBitmapString(str, 220, 1000-Y_STEP_PIN*7);
    
//    X.draw();
//    Y.draw();
//    Z.draw();
//    INK.draw();
}

void arduinoThread::digitalPinChanged(const int & pinNum) {
    // note: this will throw tons of false positives on a bare mega, needs resistors
//    cout << "pinNum: " << pinNum << endl;
//    if (pinNum == X_LIMIT_PIN) {
//        if (x_homing) {
//            X.stop();
//            x_homing = false;
//        }
//        X.stop();
//        if (x_homing) {
//            Y.ready(-100000, DELAY_MIN);
//            Y.start();
//        } else {
//            X.freeze();
//        }
//    } else if (pinNum == Z_LIMIT_PIN) {
//        Z.stop();
//        if (curState != HOMING) {
//            Z.freeze();
//        } else {
//            curState = HOME;
//        }
//    } else if (pinNum == Y_LIMIT_PIN) {
//        Y.stop();
//        if (curState == HOMING) {
//            Z.ready(-100000, DELAY_MIN);
//            Z.start();
//        } else {
//            Y.freeze();
//        }
//    } else if (pinNum == INK_LIMIT_PIN) {
//        INK.freeze();
//    }
}






