/**
 * @file lv_demos.c
 *
 */
/*********************
 *      INCLUDES
 *********************/
#include "lv_demos.h"
/*********************
 *      DEFINES
 *********************/
#define LV_DEMOS_COUNT (sizeof(demos_entry_info) / sizeof(demo_entry_info_t) - 1)
/**********************
 *      TYPEDEFS
 **********************/
typedef void (*demo_method_cb)(void);
typedef struct  {
    const char * name;
    demo_method_cb entry_cb;
} demo_entry_info_t;
/**********************
 *  STATIC VARIABLES
 **********************/
static const demo_entry_info_t demos_entry_info[] = {
#if LV_USE_DEMO_WIDGETS
    { "widgets", .entry_cb = lv_demo_widgets },
#endif
    { "", .entry_cb = NULL }
};
/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lv_demo_args_init(lv_demo_args_t * args)
{
    LV_ASSERT_NULL(args);
    lv_memzero(args, sizeof(lv_demo_args_t));
    args->parent = lv_screen_active();
}
bool lv_demos_create(char * info[], int size)
{
    const int demos_count = LV_DEMOS_COUNT;
    if(demos_count <= 0) {
        LV_LOG_ERROR("Please enable some lv_demos firstly!");
        return false;
    }
    const demo_entry_info_t * entry_info = NULL;
    if(size <= 0) {
        entry_info = &demos_entry_info[0];
    }
    else if(entry_info == NULL && info) {
        const char * name = info[0];
        for(int i = 0; i < demos_count; i++) {
            if(lv_strcmp(name, demos_entry_info[i].name) == 0) {
                entry_info = &demos_entry_info[i];
            }
        }
    }
    if(entry_info == NULL) {
        LV_LOG_ERROR("lv_demos create(%s) failure!", size > 0 ? info[0] : "");
        return false;
    }
    if(entry_info->entry_cb) {
        entry_info->entry_cb();
        return true;
    }
    return false;
}
void lv_demos_show_help(void)
{
    int i;
    const int demos_count = LV_DEMOS_COUNT;
    if(demos_count == 0) {
        LV_LOG("lv_demos: no demo available!\n");
        return;
    }
    LV_LOG("\nUsage: lv_demos demo [parameters]\n");
    LV_LOG("\ndemo list:\n");
    for(i = 0; i < demos_count; i++) {
        LV_LOG("     %s \n", demos_entry_info[i].name);
    }
}
