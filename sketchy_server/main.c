#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>

#define NANOSVG_IMPLEMENTATION  // Expands implementation
#include "nanosvg.h"

#include <sys/types.h>
#include <sys/stat.h>
#include "mongoose/mongoose.h"
#include "sketchy-ipc.h"
#include "Config.h"
#include "machine-settings.h"


static const char *s_no_cache_header =
    "Cache-Control: max-age=0, post-check=0, "
    "pre-check=0, no-store, no-cache, must-revalidate\r\n";

static int is_valid_job(struct mg_connection *conn){
    int w = Config_canvasWidth();
    int h = Config_canvasHeight();
    if (w > MAX_CANVAS_SIZE_X || h > MAX_CANVAS_SIZE_Y){
        mg_printf_data(conn, "{ \"status\": \"failed\" , \"call\" : \"start-preview\" , \"msg\" : \"The drawing size is bigger than the machine can handle. (max size width x height is %3.2f x %3.2f)\"}", MAX_CANVAS_SIZE_X, MAX_CANVAS_SIZE_Y);
        return 0;
    }

    if(access("job/manifest.ini", F_OK ) == -1 ) {
        mg_printf_data(conn, "{ \"status\": \"failed\" , \"call\" : \"start-preview\" , \"msg\" : \"The manifest.ini file is not found.\"}");
        return 0;
    }

    char jobname[50];
    sprintf(jobname,"job/%s",Config_getJob());
    if(access(jobname, F_OK ) == -1 ) {
        mg_printf_data(conn, "{ \"status\": \"failed\" , \"call\" : \"start-preview\" , \"msg\" : \"The drawing is not found, plase try to upload the drawing again.\"}");
        return 0;
    }

    return 1;
}

static void handle_status_call(struct mg_connection *conn) {
    DriverState *state = driverState();
    mg_printf_data(conn,   
                "{\"status\": \"success\", "
                            "\"call\" : \"status\", "
                            "\"name\": \"%s\", "
                            "\"statusCode\": %i, "
                            "\"messageID\": %i, "
                            "\"joburl\": \"%s\"}\n",  state->name,
                                                    state->statusCode,
                                                    state->messageID,
                                                    state->joburl);

}

static void update_config(){
    struct NSVGimage* image;
    image = nsvgParseFromFile("job/job.svg", "px", 96);
    Config_setCanvasWidth(image->width);
    Config_setCanvasHeight(image->height);
    Config_setSVGJob("job.svg");
    Config_write("job/manifest.ini");
}

static void handle_svg_call(struct mg_connection *conn) {
    DriverState *state = driverState();
    char image_url[255];
    mg_get_var(conn, "img_url", image_url, sizeof(image_url));
    char system_cmd[300] = "";
    sprintf(system_cmd,"wget -O job/job.svg %s",image_url); 
    system(system_cmd);

    update_config();

    char message[300]="";
    sprintf(message, "{\"status\": \"success\", "
                           "\"call\" : \"svg\", "
                           "\"statusCode\": %i}\n",
                                   state->statusCode);
    int message_len = strlen(message);
    mg_printf(conn,        "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\n"
                           "Cache-Control: no-cache\n"
                           "Access-Control-Allow-Origin: *\n"
                           "Content-Length: %i\n\n"
               "%s",message_len,message);
}

static int read_settings_ini(){
    char *inifile = "job/manifest.ini";
    if (Config_setIniBasePath(inifile) == -1){
        printf("%s inifile name to long.\n", inifile);
        return 1;
    }
    Config_load(inifile);
    return 1;
}

static int handle_settings_update(struct mg_connection *conn){

    //speeds
    char mindelay[100];
    char maxdelay[100];
    char minmovedelay[100];
    mg_get_var(conn, "min_delay", mindelay, sizeof(mindelay));
    mg_get_var(conn, "min_move_delay", minmovedelay, sizeof(minmovedelay));
    mg_get_var(conn, "max_delay", maxdelay, sizeof(maxdelay));
    if(maxdelay[0] != '\0'){
        Config_setMaxDelay(atoi(maxdelay));
    }
    if(mindelay[0] != '\0'){
        Config_setMinDelay(atoi(mindelay));
    }
    if(minmovedelay[0] != '\0'){
        Config_setMinMoveDelay(atoi(minmovedelay));
    }

    //sizes
    char canvas_width[100];
    char canvas_height[100];
    mg_get_var(conn, "canvas_width", canvas_width, sizeof(canvas_width));
    mg_get_var(conn, "canvas_height", canvas_height, sizeof(canvas_height));
    if(canvas_width[0] != '\0'){
        Config_setCanvasWidth(atoi(canvas_width));
    }
    if(canvas_height[0] != '\0'){
        Config_setCanvasHeight(atoi(canvas_height));
    }

    //pen speed strategy
    char pen_lookahead[100];
    mg_get_var(conn, "pen_lookahead", pen_lookahead, sizeof(pen_lookahead));
    if(pen_lookahead[0] != '\0'){
        Config_setUsePenChangeInLookAhead(atoi(pen_lookahead));
    }

    //lookahead mm
    char lookahead_mm[100];
    mg_get_var(conn, "lookahead_mm", lookahead_mm, sizeof(lookahead_mm));
    if(lookahead_mm[0] != '\0'){
        Config_setLookaheadMM(atoi(lookahead_mm));
    }

    Config_write("job/manifest.ini");
    return MG_TRUE;
}

static int handle_job_upload(struct mg_connection *conn) {

    const char *data;
    int data_len, ofs = 0;
    char var_name[100], file_name[100];

    while ((ofs = mg_parse_multipart(conn->content + ofs, conn->content_len - ofs,
                                        var_name, sizeof(var_name),
                                        file_name, sizeof(file_name),
                                        &data, &data_len)) > 0) {
        //mg_printf_data(conn, "var: %s, file_name: %s, size: %d bytes<br>",
        //               var_name, file_name, data_len);
    }

    //check if extension is either .svg // .lua // .ini // else .sketchy
    //.svg rename to job.svg
    //.lua rename to lua.svg
    //.ini thsts the mainfest referring to either svg or lua
    //.sketchy is the archive workflow containing both (manifest and source file) 
    struct stat st = {0};

    if (stat("job", &st) == -1) {
        mkdir("job", 0700);
    }

    //file_name
    char *ext = strrchr(file_name, '.');
    char *check_ini = "ini";
    char *check_svg = "svg";
    char *check_lua = "lua";
    if (!ext) {
     /* no extension */
    } else {

         if (strcmp(check_ini, ext+1)==0){

            FILE *fp = fopen("job/manifest.ini","w");
            fwrite(data, 1, data_len, fp);
            fclose(fp);

            read_settings_ini();

         }else if(strcmp(check_svg, ext+1)==0){

            FILE *fp = fopen("job/job.svg","w");
            fwrite(data, 1, data_len, fp);
            fclose(fp);

            update_config();

         }else if(strcmp(check_lua, ext+1)==0){

            FILE *fp = fopen("job/job.lua","w");
            fwrite(data, 1, data_len, fp);
            fclose(fp);
            Config_setLuaJob("job.lua");
            Config_write("job/manifest.ini");

         }else{

            system("exec rm -r job/*");
            FILE *fp = fopen("job/sketchy-job.tar.gz","w");
            fwrite(data, 1, data_len, fp);
            fclose(fp);
            system("tar -xf job/sketchy-job.tar.gz -C job");
            system("exec rm job/sketchy-job.tar.gz");

         }
    }
    return MG_TRUE;
}

static void handle_preview_status_call(struct mg_connection *conn){
    DriverState *state = driverState();
    if(state->statusCode == driverSatusCodeIdle){
        mg_printf_data(conn, "{ \"status\": \"success\", \"call\" : \"preview-status\", \"msg\":\"DONE\"}");
        return;
    }else{
        mg_printf_data(conn, "{ \"status\": \"success\", \"call\" : \"preview-status\", \"msg\":\"BUSSY\"}");
        return;
    }
}

static void handle_preview_abort_call(struct mg_connection *conn){

    setCommand("preview-abort",commandCodePreviewAbort,0.0,0);
    mg_printf_data(conn, "{ \"status\": \"success\" , \"call\" : \"preview-abort\"}");

}

static void handle_preview_call(struct mg_connection *conn){

    if(!is_valid_job(conn)){
        return;
    }

    DriverState *state = driverState();
    if(state->statusCode == driverSatusCodeBusy){
        mg_printf_data(conn, "{ \"status\": \"failed\", \"call\" : \"start\", \"msg\":\"Sketchy is busy\"}");
        return;
    }

    int status;
    if(fork() == 0){ 
        // Child process will return 0 from fork()
        status = system("./sketchy-preview job/manifest.ini");
        if(status != -1){
            //do something?
        }
        exit(0);
    }else{
        // Parent process will return a non-zero value from fork()
    }

    mg_printf_data(conn, "{ \"status\": \"success\" , \"call\" : \"preview\"}");

}

static void handle_start_call(struct mg_connection *conn) {

    if(!is_valid_job(conn)){
        return;
    }

    DriverState *state = driverState();
    if(state->statusCode == driverSatusCodeBusy){
        mg_printf_data(conn, "{ \"status\": \"failed\", \"call\" : \"start\", \"msg\":\"Sketchy is busy\"}");
        return;
    }

    int status;
    if(fork() == 0){ 
        // Child process will return 0 from fork()
        status = system("./sketchy-driver job/manifest.ini");
        if(status != -1){
            //do something?
        }
        exit(0);
    }else{
        // Parent process will return a non-zero value from fork()
    }

    mg_printf_data(conn, "{ \"status\": \"success\" , \"call\" : \"start\"}");

}

static void handle_ini_call(struct mg_connection *conn){
    const char* json = Config_getJSON();
    mg_printf_data(conn, json);
}

static void handle_pause_call(struct mg_connection *conn) {
    DriverState *state = driverState();
    if(state->statusCode != driverSatusCodeBusy){
        mg_printf_data(conn, "{ \"status\": \"failed\" , \"call\" : \"pause\", \"msg\":\"Can not pause, machine is not running.\"}");
        return;
    }
    setCommand("pause",commandCodePause,0.0,0);
    mg_printf_data(conn, "{ \"status\": \"success\" , \"call\" : \"pause\"}");
}

static void handle_resume_call(struct mg_connection *conn) {
    DriverState *state = driverState();
    if(state->statusCode != driverStatusCodePaused){
        mg_printf_data(conn, "{ \"status\": \"failed\" , \"call\" : \"resume\", \"msg\":\"Can not resume, machine is not paused.\"}");
        return;
    }
    setCommand("resume",commandCodeNone,0.0,0);
    mg_printf_data(conn, "{ \"status\": \"success\" , \"call\" : \"resume\"}");
}

static void handle_stop_call(struct mg_connection *conn) {
    setCommand("stop",commandCodeStop,0.0,0);
    mg_printf_data(conn, "{ \"status\": \"success\" , \"call\" : \"stop\"}");
}

static void handle_reset_call(struct mg_connection *conn) {
    system("ipcrm -M 1234");
    system("ipcrm -M 4567");
    shmCreate();
    mg_printf_data(conn, "{ \"status\": \"success\" , \"call\" : \"reset\"}");
}

static int ev_handler(struct mg_connection *conn, enum mg_event ev) {

    switch (ev) {
        case MG_AUTH: return MG_TRUE;
        case MG_REQUEST:

            if(!strcmp(conn->uri, "/handle_post_request")){
                handle_job_upload(conn);
            }

            if(!strcmp(conn->uri, "/handle_settings_update")){
                handle_settings_update(conn);
            }

            if(!strcmp(conn->uri, "/api/resetshm")){
                handle_reset_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/start")){
                handle_start_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/preview")){
                handle_preview_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/preview-status")){
                handle_preview_status_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/preview-abort")){
                handle_preview_abort_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/ini")){
                handle_ini_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/stop")){
                handle_stop_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/status")){
                handle_status_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/pause")){
                handle_pause_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/api/resume")){
                handle_resume_call(conn);
                return MG_TRUE;
            }
        
            if(!strcmp(conn->uri, "/api/svg_url")){
                handle_svg_call(conn);
                return MG_TRUE;
            }

            if(!strcmp(conn->uri, "/job/manifest.ini")){
                mg_send_file(conn, "job/manifest.ini", s_no_cache_header);
                return MG_MORE;
            }

            if(!strcmp(conn->uri, "/preview-img")){
                mg_send_file(conn, "preview_image.png", s_no_cache_header);
                return MG_MORE;
            }

            if(!strcmp(conn->uri, "/job")){
                char jobname[50];
                sprintf(jobname,"job/%s",Config_getJob());
                if(access(jobname, F_OK ) != -1 ) {
                    mg_send_file(conn, jobname, s_no_cache_header);
                }
                return MG_MORE;
            }

            mg_send_file(conn, "index.html", s_no_cache_header);
            return MG_MORE;
        default: return MG_FALSE;
    }
}

int main(void) {

    printf("Build with max canvas size: %f x %f\n",MAX_CANVAS_SIZE_X,MAX_CANVAS_SIZE_Y);

    read_settings_ini();
    
    shmCreate();

    struct mg_server *server;

    // Create and configure the server
    server = mg_create_server(NULL, ev_handler);
    mg_set_option(server, "listening_port", "8000");

    // Serve request. Hit Ctrl-C to terminate the program
    printf("Starting on port %s\n", mg_get_option(server, "listening_port"));
    for (;;) {
        mg_poll_server(server, 1000);
    }

    // Cleanup, and free server instance
    mg_destroy_server(&server);

    return 0;
}
