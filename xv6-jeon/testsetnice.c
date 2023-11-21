#include "types.h"
#include "user.h"
#include "stat.h"

int main()
{
    int result;
    result = setnice(3, 11);
    if (result == 0){
        printf(1, "setnice success\n");
    }
    else if (result == -1){
        printf(1, "setnice failed\n");
    }

    result = setnice(1, -1);
    if (result == 0){
        printf(1, "setnice success\n");
    }
    else if (result == -1){
        printf(1, "setnice failed\n");
    }
    exit();
}