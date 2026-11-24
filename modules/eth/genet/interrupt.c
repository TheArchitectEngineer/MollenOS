/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * GENET packet interrupt: the fast handler, its setup and its teardown. The
 * fast handler does very little. It switches the causes off (masks them) and
 * writes them down. All the work on the frame rings happens later, in the
 * module's normal context (service.c).
 */

#include <ddk/utils.h>
#include <errno.h>
#include <event.h>
#include <io.h>
#include <ioctl.h>
#include <ioset.h>

#include "genet.h"

irqstatus_t
GenetFastInterrupt(
    _In_ InterruptFunctionTable_t* interruptTable,
    _In_ void*                     context)
{
    InterruptResourceTable_t* resources = context;
    GenetInterruptShared_t*   shared;
    DeviceIo_t*               registers;
    uint32_t                  pending;
    uint32_t                  causes;
    oserr_t                   signalStatus;
    unsigned int              failures = 0;

    // This handler can only reach the resources that were registered for it.
    // Resource 0 is the register window, and memory resource 0 is the shared
    // state.
    registers = INTERRUPT_IOSPACE(resources, 0);
    shared = (GenetInterruptShared_t*)INTERRUPT_RESOURCE(resources, 0);

    // The interrupt line stays active for as long as any enabled cause is set.
    // Every such cause has to be silenced here, not only the packet causes,
    // or the handler would be called again straight away.
    pending = (uint32_t)interruptTable->ReadIoSpace(registers, GENET_INTR0_STATUS, GENET_REGISTER_SIZE);
    pending &= ~(uint32_t)interruptTable->ReadIoSpace(registers, GENET_INTR0_MASK_STATUS, GENET_REGISTER_SIZE);
    if (pending == 0) {
        return IRQSTATUS_NOT_HANDLED;
    }

    // Mask first, then clear. A cause that fires after the clear stays set in
    // the status register and raises a new interrupt once it is unmasked.
    // This handler cannot log, because message text lives outside the memory
    // it can reach. Failures are counted instead, and the module reports them.
    if (interruptTable->WriteIoSpace(registers, GENET_INTR0_MASK_SET, pending, GENET_REGISTER_SIZE) != OS_EOK) {
        failures++;
    }
    if (interruptTable->WriteIoSpace(registers, GENET_INTR0_CLEAR, pending, GENET_REGISTER_SIZE) != OS_EOK) {
        failures++;
    }

    // Reading a register back makes sure both writes reached the device before
    // the interrupt controller looks at the line again.
    (void)interruptTable->ReadIoSpace(registers, GENET_INTR0_MASK_STATUS, GENET_REGISTER_SIZE);

    // A lost signal does no lasting harm. The module's regular poll also picks
    // up the recorded causes. EventSignal returns OS_EINCOMPLETE when it raised
    // the event counter, and OS_EOK when the counter was already raised by an
    // earlier signal. Both mean the event is signaled.
    causes = pending & GENET_INTR0_DMA_DONE;
    if (causes != 0) {
        atomic_fetch_or(&shared->Causes, causes);
        signalStatus = interruptTable->EventSignal(resources->HandleResource);
        if (signalStatus != OS_EOK && signalStatus != OS_EINCOMPLETE) {
            failures++;
        }
    }

    if (failures != 0) {
        atomic_fetch_add(&shared->Failures, failures);
    }
    return IRQSTATUS_HANDLED;
}

static oserr_t
__EventOpen(
    _InOut_ GenetInterrupt_t* interrupt)
{
    int nonBlocking = 1;
    int result;

    interrupt->EventDescriptor = eventd(0, EVT_RESET_EVENT);
    if (interrupt->EventDescriptor < 0) {
        ERROR("GENET interrupt: creating the event descriptor failed");
        return OS_EUNKNOWN;
    }

    // The service reads the descriptor while it holds the adapter lock, so it
    // must never block waiting for a signal.
    result = ioctl(interrupt->EventDescriptor, FIONBIO, &nonBlocking);
    if (result) {
        ERROR("GENET interrupt: making the event descriptor non-blocking failed: %i", result);
        result = close(interrupt->EventDescriptor);
        if (result) {
            WARNING("GENET interrupt: closing the event descriptor failed: %i", result);
        }
        return OS_EUNKNOWN;
    }

    result = ioset_ctrl(
        gracht_server_get_aio_handle(EthernetServer()),
        IOSET_ADD,
        interrupt->EventDescriptor,
        &(struct ioset_event) {
            .data.iod = interrupt->EventDescriptor,
            .events = IOSETSYN
        }
    );
    if (result) {
        ERROR("GENET interrupt: adding the event descriptor to the I/O set failed: %i", result);
        result = close(interrupt->EventDescriptor);
        if (result) {
            WARNING("GENET interrupt: closing the event descriptor failed: %i", result);
        }
        return OS_EUNKNOWN;
    }

    interrupt->State = GENET_INTERRUPT_EVENT_READY;
    return OS_EOK;
}

static void
__EventClose(
    _InOut_ GenetController_t* controller)
{
    GenetInterrupt_t* interrupt = &controller->Interrupt;
    int               result;

    if (interrupt->State == GENET_INTERRUPT_NONE) {
        return;
    }

    // If either call fails, all we can do is report it. The descriptor cannot
    // be used afterwards in any case, and the interrupt that signaled it is
    // already destroyed.
    result = ioset_ctrl(
        gracht_server_get_aio_handle(EthernetServer()),
        IOSET_DEL,
        interrupt->EventDescriptor,
        &(struct ioset_event) {
            .data.iod = interrupt->EventDescriptor,
            .events = IOSETSYN
        }
    );
    if (result) {
        WARNING("GENET %u interrupt: removing the event descriptor from the I/O set failed: %i",
            controller->Device->Id, result);
    }

    result = close(interrupt->EventDescriptor);
    if (result) {
        WARNING("GENET interrupt: closing the event descriptor failed: %i", result);
    }
    interrupt->State = GENET_INTERRUPT_NONE;
}

oserr_t
GenetInterruptRegister(
    _InOut_ GenetController_t* controller)
{
    GenetInterrupt_t* interrupt = &controller->Interrupt;
    uint32_t          count;
    oserr_t           status;

    // GenetMacInitialize masked the causes, and they stay masked until the
    // queues start. Turning on delivery here therefore cannot raise an interrupt.
    atomic_init(&interrupt->Shared.Causes, 0);
    atomic_init(&interrupt->Shared.Failures, 0);

    ETHERNET_LOG("GENET interrupt: event descriptor");
    status = __EventOpen(interrupt);
    if (status != OS_EOK) {
        return status;
    }

    ETHERNET_LOG("GENET interrupt: reserve source %u", GENET_PACKET_INTERRUPT_INDEX);
    DeviceInterruptInitialize(&interrupt->Source, controller->Device);
    status = DeviceInterruptSelectSource(&interrupt->Source, GENET_PACKET_INTERRUPT_INDEX);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: selecting source %u failed: %u", GENET_PACKET_INTERRUPT_INDEX, status);
        return status;
    }

    status = DeviceInterruptAllocate(&interrupt->Source, 1, 1, &count);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: reserving source %u failed: %u", GENET_PACKET_INTERRUPT_INDEX, status);
        return status;
    }

    // Every setter below refers to vector 0, which is the only vector a platform
    // interrupt source has.
    status = DeviceInterruptSetDescriptor(&interrupt->Source, 0, interrupt->EventDescriptor);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: setting the event descriptor failed: %u", status);
        return status;
    }

    status = DeviceInterruptSetHandler(&interrupt->Source, 0, GenetFastInterrupt);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: setting the fast handler failed: %u", status);
        return status;
    }

    status = DeviceInterruptAddIoResource(&interrupt->Source, 0, &controller->Registers);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: adding the register window failed: %u", status);
        return status;
    }

    status = DeviceInterruptAddMemoryResource(
        &interrupt->Source,
        0,
        (uintptr_t)&interrupt->Shared,
        sizeof(interrupt->Shared),
        0
    );
    if (status != OS_EOK) {
        ERROR("GENET interrupt: adding the shared state failed: %u", status);
        return status;
    }

    ETHERNET_LOG("GENET interrupt: enable");
    status = DeviceInterruptEnable(&interrupt->Source);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: enabling delivery failed: %u", status);
        return status;
    }

    interrupt->State = GENET_INTERRUPT_ENABLED;
    return OS_EOK;
}

oserr_t
GenetInterruptUnregister(
    _InOut_ GenetController_t* controller)
{
    GenetInterrupt_t* interrupt = &controller->Interrupt;
    oserr_t           status;

    // Stop the device from raising causes before the interrupt goes away, since
    // a shared line may stay enabled. A failed mask is not fatal, because the
    // destroy call below stops delivery at the interrupt controller anyway.
    if (GenetRegistersMapped(controller)) {
        status = GenetMacMaskInterrupts(controller);
        if (status != OS_EOK) {
            WARNING("GENET interrupt: masking causes before release failed: %u", status);
        }
    }

    // Destroy waits for a running handler to finish. The event descriptor and
    // the shared state must stay valid until it succeeds, so the descriptor is
    // closed only after that.
    status = DeviceInterruptDestroy(&interrupt->Source);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: release failed, keeping its resources: %u", status);
        return status;
    }

    __EventClose(controller);
    return OS_EOK;
}

bool
GenetInterruptIsEvent(
    _In_ const GenetController_t* controller,
    _In_ int                      descriptor)
{
    return controller->Interrupt.State != GENET_INTERRUPT_NONE &&
           controller->Interrupt.EventDescriptor == descriptor;
}

uint32_t
GenetInterruptTakeCauses(
    _InOut_ GenetController_t* controller)
{
    GenetInterrupt_t* interrupt = &controller->Interrupt;
    unsigned int      failures;
    uint32_t          causes;

    // Without a registered handler nothing writes the shared state, and both
    // values stay zero.
    causes = atomic_exchange(&interrupt->Shared.Causes, 0);
    failures = atomic_exchange(&interrupt->Shared.Failures, 0);
    if (failures != 0) {
        ERROR("GENET interrupt: the fast handler failed %u register or signal operations", failures);
    }
    if (causes != 0) {
        GenetLogInterrupt(controller, causes);
    }
    return causes;
}

void
GenetInterruptResetEvent(
    _InOut_ GenetController_t* controller)
{
    unsigned int signal;
    int          raised = 0;

    // The counter is 0 or 1. Only a signal that raises it queues an I/O set
    // event. A handler on another core can raise the counter just before we
    // read it and queue its event just after. We then take its causes in this
    // pass, and its event finds the counter at 0, so an empty counter is
    // normal. Only this function reads the counter, under the adapter lock,
    // and the descriptor is non-blocking.
    if (ioctl(controller->Interrupt.EventDescriptor, FIONREAD, &raised)) {
        WARNING("GENET interrupt: querying the event descriptor failed: %i", errno);
        return;
    }
    if (raised <= 0) {
        return;
    }

    // A raised counter that cannot be read is a fault. libc does not set
    // errno when this read fails, so errno is cleared first.
    errno = 0;
    if (read(controller->Interrupt.EventDescriptor, &signal, sizeof(signal)) != sizeof(signal)) {
        WARNING("GENET interrupt: reading the raised event descriptor failed: %i", errno);
    }
}
