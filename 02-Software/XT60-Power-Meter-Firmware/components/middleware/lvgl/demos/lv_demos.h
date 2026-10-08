/**
 * @file lv_demos.h
 *
 */
#ifndef LV_DEMOS_H
#define LV_DEMOS_H
#ifdef __cplusplus
extern "C" {
#endif
/*********************
 *      INCLUDES
 *********************/
#include "../include/lvgl/lvgl.h"
typedef struct _lv_demo_args lv_demo_args_t;
#if LV_USE_DEMO_WIDGETS
#include "widgets/lv_demo_widgets.h"
#endif
/*********************
 *      DEFINES
 *********************/
/**********************
 *      TYPEDEFS
 **********************/
struct _lv_demo_args {
    lv_obj_t * parent;
};
/**********************
 * GLOBAL PROTOTYPES
 **********************/
void lv_demo_args_init(lv_demo_args_t * args);
bool lv_demos_create(char * info[], int size);
void lv_demos_show_help(void);
/**********************
 *      MACROS
 **********************/
#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /*LV_DEMOS_H*/
