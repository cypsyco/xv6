#include "types.h"
#include "user.h"
#include "stat.h"

int main()
{
    int result;
    result = getnice(1);
    if (result== -1){
        printf(1, "getnice failed\n");
    }
    else{
        printf(1, "getnice success, nice value: %d\n", result);
    }
    exit();
}