/* AxisOS - CMOS real time clock */
#include "axis.h"

static uint8_t cmos(uint8_t r) {
    outb(0x70, r);
    return inb(0x71);
}
static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_read(rtc_time_t *t) {
    while (cmos(0x0A) & 0x80) {}
    int sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    int day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    uint8_t b = cmos(0x0B);
    if (!(b & 0x04)) {
        sec = bcd(sec);
        min = bcd(min);
        hour = bcd(hour & 0x7F) | (hour & 0x80);
        day = bcd(day);
        mon = bcd(mon);
        year = bcd(year);
    }
    if (!(b & 0x02) && (hour & 0x80)) hour = ((hour & 0x7F) + 12) % 24;
    t->sec = sec;
    t->min = min;
    t->hour = hour & 0x7F;
    t->day = day;
    t->month = mon;
    t->year = 2000 + year;
    rtc_from_unix(rtc_unix(t), t); /* computes weekday */
}

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

uint32_t rtc_unix(const rtc_time_t *t) {
    uint32_t days = 0;
    for (int y = 1970; y < t->year; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < t->month; m++) days += mdays[m - 1] + (m == 2 && leap(t->year));
    days += t->day - 1;
    return days * 86400u + t->hour * 3600u + t->min * 60u + t->sec;
}

void rtc_from_unix(uint32_t ts, rtc_time_t *t) {
    uint32_t days = ts / 86400u, rem = ts % 86400u;
    t->wday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    t->hour = rem / 3600;
    t->min = (rem % 3600) / 60;
    t->sec = rem % 60;
    int y = 1970;
    for (;;) {
        uint32_t yd = leap(y) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    t->year = y;
    int m = 0;
    for (;;) {
        uint32_t md = mdays[m] + (m == 1 && leap(y));
        if (days < md) break;
        days -= md;
        m++;
    }
    t->month = m + 1;
    t->day = (int)days + 1;
}

void rtc_local(rtc_time_t *t) {
    rtc_read(t);
    int32_t ts = (int32_t)rtc_unix(t) + cfg.tz_hours * 3600;
    rtc_from_unix((uint32_t)ts, t);
}

const char *rtc_wday_name(int w) {
    static const char *n[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return n[w % 7];
}
const char *rtc_month_name(int m) {
    static const char *n[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return n[(m - 1) % 12];
}
