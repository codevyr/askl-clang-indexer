#ifndef REGISTER_H
#define REGISTER_H

struct driver_info { int id; };

/* Macro that references its argument — mirrors module_pci_driver() */
#define register_driver(drv) \
    static int __init(void) { return (drv).id; } \
    static void __exit(void) { (void)(drv).id; }

#endif
