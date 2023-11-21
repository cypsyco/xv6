#include "types.h"
#include "user.h"
#include "stat.h"

int main()
{
    setnice(1, 11);
    setnice(2, 11);
    setnice(3, 11);
    ps(0);
    exit();
}