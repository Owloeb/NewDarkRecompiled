/* com.h: guest-visible COM objects for the DirectX front ends.
 *
 * A COM object the game holds is 16 bytes of guest memory: [vtable, 'COM!', host id, 0]. Its vtable lives in guest
 * memory too and every slot is a host thunk; the slot layout and the number of arguments each method pops come from
 * ifaces.h, which is generated from the real DirectX headers, so a front end only writes the methods it implements
 * and can never get a slot or a stack cleanup wrong. Unimplemented methods return the class's default result. */
#ifndef DX_COM_H
#define DX_COM_H
#include "core.h"
#include "ifaces.h"

typedef struct ComObj ComObj;
typedef void (*com_method)(CPU *c, ComObj *self);    /* arguments: A(1), A(2)... (A(0) is `this`); result in eax */
typedef struct { const char *name; com_method fn; } ComMethod;
typedef struct ComClass {
    const char *name;                 /* interface name, for logs */
    const char *spec;                 /* IFACE_xxx from ifaces.h */
    const uint8_t (*iids)[16];        /* interfaces QueryInterface answers with this same object (IUnknown always) */
    int niids;
    const ComMethod *methods;         /* NULL-terminated */
    uint32_t default_hr;              /* returned by methods that are not implemented (S_OK unless noted) */
    void (*destroy)(ComObj *self);    /* the last Release */
    /* filled at first use */
    uint32_t vtable; int nslots; com_method *slot_fn; int index;
} ComClass;
struct ComObj {
    ComClass *cls;
    uint32_t guest;                   /* the object as the game sees it */
    int refs;
    void *data;                       /* the front end's state */
    ComObj *extra_iface_of;           /* for aggregated interfaces (a 3D buffer belongs to its sound buffer) */
};
ComObj  *com_create(ComClass *cls, void *data);        /* refs = 1 */
ComObj  *com_get(uint32_t guest);                      /* NULL if not one of ours */
ComObj  *com_get_as(uint32_t guest, ComClass *cls);    /* NULL unless guest is an object of cls */
void     com_addref(ComObj *o);
void     com_release(ComObj *o);
int      com_iid_eq(uint32_t guest_iid, const uint8_t iid[16]);
#define COM_IID(x) ((const uint8_t[16])x)
#define S_OK_ 0u
#define E_NOINTERFACE_ 0x80004002u
#define E_NOTIMPL_ 0x80004001u
#define E_INVALIDARG_ 0x80070057u
#define E_FAIL_ 0x80004005u
#define OUTP(i, v) do { uint32_t p_ = A(i); if (p_) WR32(p_, (v)); } while (0)
#endif
