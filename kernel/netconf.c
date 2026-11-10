/* AxisOS - Network settings screen (also the 'netconf' command) */
#include "axis.h"

/* from net.c */
extern uint32_t net_ip, net_mask, net_gw, net_dns;
bool net_present(void);
bool net_up(void);
bool net_renew(void);
const char *net_nic_name(void);
void net_set_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
void net_set_dhcp(void);
bool net_is_dhcp(void);
void ip_to_str(uint32_t ip, char *out);
bool str_to_ip(const char *s, uint32_t *ip);

int tui_prompt(const char *title, const char *label, char *buf, size_t n);

enum { N_MODE, N_IP, N_MASK, N_GW, N_DNS, N_APPLY, N_RENEW, N_CLOSE, N_ROWS };
static const char *labels[N_ROWS] = {
    "Configuration", "IP address", "Subnet mask", "Gateway", "DNS server",
    "> Apply", "> Renew / reconnect (DHCP)", "> Close"};

static void draw(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns, bool dhcp, int sel, const char *msg) {
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
    scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
    scr_puts(1, 0, "Network Settings", theme.bar);
    scr_printf(2, 2, theme.accent, "Adapter: %s", net_present() ? net_nic_name() : "none found");
    scr_printf(2, 3, theme.dim, "Status:  %s", !net_present() ? "no network card" : (net_up() ? "connected" : "no address"));
    char macs[18];
    extern uint8_t net_mac[6];
    snprintf(macs, sizeof macs, "%02x:%02x:%02x:%02x:%02x:%02x", net_mac[0], net_mac[1], net_mac[2], net_mac[3], net_mac[4], net_mac[5]);
    scr_printf(40, 3, theme.dim, "MAC: %s", macs);

    char v[20];
    for (int i = 0; i < N_ROWS; i++) {
        int y = 5 + i + (i >= N_APPLY ? 1 : 0);
        uint8_t a = i == sel ? theme.select : theme.normal;
        scr_fill(2, y, 72, 1, ' ', a);
        scr_puts(4, y, labels[i], a);
        v[0] = 0;
        if (i == N_MODE) strcpy(v, dhcp ? "Automatic (DHCP)" : "Manual (static)");
        else if (i == N_IP) ip_to_str(ip, v);
        else if (i == N_MASK) ip_to_str(mask, v);
        else if (i == N_GW) ip_to_str(gw, v);
        else if (i == N_DNS) ip_to_str(dns, v);
        if (v[0]) {
            bool dim = dhcp && (i >= N_IP && i <= N_DNS);
            scr_printf(44, y, dim ? theme.dim : a, i == N_MODE ? "< %s >" : "[ %s ]", v);
        }
    }
    scr_fill(0, 22, SCREEN_W, 1, ' ', theme.normal);
    scr_putsn(2, 22, msg && *msg ? msg : "Automatic asks a router for an address. Manual lets you type a fixed one.",
              76, msg && *msg ? theme.accent : theme.dim);
    scr_fill(0, 24, SCREEN_W, 1, ' ', theme.bar);
    scr_puts(1, 24, "Up/Down choose   Left/Right or Enter change   A apply   Esc close", theme.bar);
}

int netconf_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    uint32_t ip = net_ip, mask = net_mask, gw = net_gw, dns = net_dns;
    bool dhcp = net_is_dhcp();
    int sel = 0;
    char msg[96] = "";
    if (!net_present()) strlcpy(msg, "No network card found. In QEMU add: -nic user,model=e1000", sizeof msg);
    for (;;) {
        draw(ip, mask, gw, dns, dhcp, sel, msg);
        msg[0] = 0;
        int k = kbd_getkey();
        if (k == KEY_ESC || k == 'q' || k == 'Q') break;
        else if (k == KEY_UP) sel = (sel + N_ROWS - 1) % N_ROWS;
        else if (k == KEY_DOWN || k == '\t') sel = (sel + 1) % N_ROWS;
        else if (k == 'a' || k == 'A') {
            sel = N_APPLY;
            k = '\n';
        }
        if (k != '\n' && k != KEY_LEFT && k != KEY_RIGHT) continue;
        if (sel == N_MODE) dhcp = !dhcp;
        else if (sel >= N_IP && sel <= N_DNS && !dhcp) {
            char buf[20];
            uint32_t *target = sel == N_IP ? &ip : sel == N_MASK ? &mask : sel == N_GW ? &gw : &dns;
            ip_to_str(*target, buf);
            if (tui_prompt(labels[sel], "Enter an address like 192.168.1.50 :", buf, sizeof buf)) {
                uint32_t val;
                if (str_to_ip(buf, &val)) *target = val;
                else strlcpy(msg, "That is not a valid IP address.", sizeof msg);
            }
            con_show_cursor(false);
        } else if (sel == N_APPLY) {
            if (!net_present()) {
                strlcpy(msg, "No network card to configure.", sizeof msg);
                continue;
            }
            if (dhcp) {
                scr_fill(0, 22, SCREEN_W, 1, ' ', theme.normal);
                scr_puts(2, 22, "Asking the network for an address...", theme.accent);
                bool ok = net_renew();
                ip = net_ip;
                mask = net_mask;
                gw = net_gw;
                dns = net_dns;
                strlcpy(msg, ok ? "Connected automatically (DHCP)." : "No DHCP server answered - check the connection.", sizeof msg);
            } else {
                if (!ip) {
                    strlcpy(msg, "Set an IP address first.", sizeof msg);
                    continue;
                }
                net_set_static(ip, mask ? mask : 0xFFFFFF00, gw, dns ? dns : gw);
                strlcpy(msg, "Static network settings applied.", sizeof msg);
            }
        } else if (sel == N_RENEW) {
            dhcp = true;
            scr_fill(0, 22, SCREEN_W, 1, ' ', theme.normal);
            scr_puts(2, 22, "Reconnecting...", theme.accent);
            bool ok = net_renew();
            ip = net_ip;
            mask = net_mask;
            gw = net_gw;
            dns = net_dns;
            strlcpy(msg, ok ? "Reconnected (DHCP)." : "No DHCP server answered.", sizeof msg);
        } else if (sel == N_CLOSE)
            break;
    }
    scr_restore(saved);
    con_show_cursor(true);
    return 0;
}
