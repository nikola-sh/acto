#include <system/platform.h>

#include "core.h"
#include "worker.h"

namespace acto {

namespace core {

using fastdelegate::FastDelegate;
using fastdelegate::MakeDelegate;


// Экземпляр runtime'а
//runtime_t   runtime;

//
static TLS_VARIABLE thread_context_t* threadCtx = 0;


//-----------------------------------------------------------------------------
void destroy_object_body(object_t* obj) {
    assert(!obj->unimpl && obj->impl);

    // TN: Эта процедура всегда должна вызываться
    //     внутри блокировки полей объекта

    obj->unimpl = true;
    delete obj->impl, obj->impl = NULL;
    obj->unimpl = false;

    if (obj->waiters) {
        object_t::waiter_t* next = NULL;
        object_t::waiter_t* it   = obj->waiters;

        while (it) {
            // TN: Необходимо читать значение следующего указателя
            //     заранее, так как пробуждение ждущего потока
            //     приведет к удалению текущего узла списка
            next = it->next;
            it->event->signaled();
            it   = next;
        }

        obj->waiters = NULL;
    }
}



///////////////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
base_t::base_t()
    : m_terminating(false)
{
    // -
}
//-----------------------------------------------------------------------------
base_t::~base_t() {
    for (Handlers::iterator i = m_handlers.begin(); i != m_handlers.end(); i++) {
        // Удалить обработчик
        if ((*i)->handler)
            delete (*i)->handler;
        // Удалить элемент списка
        delete (*i);
    }
}
//-----------------------------------------------------------------------------
void base_t::terminate() {
    this->m_terminating = true;
}
//-----------------------------------------------------------------------------
void base_t::set_handler(i_handler* const handler, const TYPEID type) {
    for (Handlers::iterator i = m_handlers.begin(); i != m_handlers.end(); ++i) {
        if ((*i)->type == type) {
            // 1. Удалить предыдущий обработчик
            if ((*i)->handler)
                delete (*i)->handler;
            // 2. Установить новый
            (*i)->handler = handler;
            // -
            return;
        }
    }
    // Запись для данного типа сообщения еще не существует
    {
        HandlerItem* const item = new HandlerItem(type);
        // -
        item->handler = handler;
        // -
        m_handlers.push_back(item);
    }
}


///////////////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
i_handler::i_handler(const TYPEID type_)
    : m_type(type_)
{
}


///////////////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
object_t::object_t(worker_t* const thread_)
    : impl      (NULL)
    , thread    (thread_)
    , waiters   (NULL)
    , references(0)
    , binded    (false)
    , deleting  (false)
    , freeing   (false)
    , scheduled (false)
    , unimpl    (false)
{
    next = NULL;
}


///////////////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
package_t::package_t(msg_t* const data_, const TYPEID type_) :
    data  (data_),
    sender(0),
    type  (type_)
{
}
//-----------------------------------------------------------------------------
package_t::~package_t() {
    // Освободить ссылки на объекты
    if (sender)
        runtime_t::instance()->release(sender);
    // -
    runtime_t::instance()->release(target);
    // Удалить данные сообщения
    delete data;
}



///////////////////////////////////////////////////////////////////////////////
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

void doHandle(package_t* const package) {
    object_t* const obj = package->target;
    i_handler* handler  = 0;
    base_t* const impl  = obj->impl;

    // 1. Найти обработчик соответствующий данному сообщению
    for (Handlers::iterator i = impl->m_handlers.begin(); i != impl->m_handlers.end(); ++i) {
        if (package->type == (*i)->type) {
            handler = (*i)->handler;
            break;
        }
    }
    // 2. Если соответствующий обработчик найден, то вызвать его
    try {
        if (handler) {
            // TN: Данный параметр читает только функция determine_sender,
            //     которая всегда выполняется в контексте этого потока.
            threadCtx->sender = obj;
            // -
            handler->invoke(package->sender, package->data);
            // -
            threadCtx->sender = 0;

            if (impl->m_terminating)
                runtime_t::instance()->destroyObject(obj);
        }
    }
    catch (...) {
        // -
    }
}

///////////////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
runtime_t::runtime_t() :
    m_active     (false),
    m_processors (1),
    m_scheduler  (0),
    m_terminating(false)
{
    m_processors = NumberOfProcessors();
}
//-----------------------------------------------------------------------------
runtime_t::~runtime_t() {
    // Удалить рабочие потоки
}
//-----------------------------------------------------------------------------
runtime_t* runtime_t::instance() {
    static runtime_t value;

    return &value;
}

///////////////////////////////////////////////////////////////////////////////
//                            PUBLIC METHODS                                 //
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// Desc: Захватить ссылку на объект
//-----------------------------------------------------------------------------
void runtime_t::acquire(object_t* const obj) {
    assert(obj != 0);
    // -
    atomic_increment(&obj->references);
}
//-----------------------------------------------------------------------------
object_t* runtime_t::createActor(base_t* const impl, const int options) {
    assert(impl != 0);

    // Флаг соблюдения предусловий
    bool        preconditions = false;
    // Зарезервированный поток
    worker_t*   worker        = 0;

    // !!! aoExclusive aoBindToThread - не могут быть вместе

    // Создать для актера индивидуальный поток
    if (options & acto::aoExclusive)
        worker = createWorker();

    // 2. Проверка истинности предусловий
    preconditions = true &&
    // Поток зарзервирован
        (options & acto::aoExclusive) ? (worker != 0) : (worker == 0);

    // 3. Создание актера
    if (preconditions) {
        try {
            // -
            object_t* const result = new core::object_t(worker);
            // -
            result->impl       = impl;
            result->references = 1;
            result->scheduled  = (worker != 0) ? true : false;
            // Зарегистрировать объект в системе
            if (options & acto::aoBindToThread) {
                result->binded = true;
                // -
                threadCtx->actors.insert(result);
            }
            else {
                Exclusive  lock(m_cs.actors);
                // -
                m_actors.insert(result);
                m_evnoactors.reset();
            }
            // -
            if (worker) {
                atomic_increment(&m_workers.reserved);
                // -
                worker->assign(result, 0);
            }
            // -
            return result;
        }
        catch (...) {
            // Поставить в очередь на удаление
            if (worker != 0)
                m_workers.deleted.push(worker);
        }
    }
    return 0;
}
//-----------------------------------------------------------------------------
void runtime_t::destroyObject(object_t* const obj) {
    assert(obj != 0);

    // -
    bool deleting = false;
    // -
    {
        Exclusive   lock(obj->cs);
        // Если объект уже удаляется, то делать более ничего не надо.
        if (!obj->deleting) {
            // Запретить более посылать сообщения объекту
            obj->deleting = true;
            // Если объект не обрабатывается, то его можно начать разбирать
            if (!obj->scheduled) {
                if (obj->impl)
                    destroy_object_body(obj);
                // -
                if (!obj->freeing && (obj->references == 0)) {
                    obj->freeing = true;
                    deleting     = true;
                }
            }
            else
                // Если это эксклюзивный объект, то разбудить соответствующий
                // поток, так как у эксклюзивных объектов всегда установлен флаг scheduled
                if (obj->thread)
                    obj->thread->wakeup();
        }
    }
    // Объект полностью разобран и не имеет ссылок - удалить его
    if (deleting)
        pushDelete(obj);
}
//-----------------------------------------------------------------------------
void runtime_t::join(object_t* const obj) {
    std::auto_ptr< object_t::waiter_t > node;
    event_t event;

    if (threadCtx->sender != obj) {
        Exclusive   lock(obj->cs);

        if (obj->impl) {
            node.reset(new object_t::waiter_t());

            node->event  = &event;
            node->next   = obj->waiters;
            obj->waiters = node.get();
            event.reset();
        }
    }
    // -
    if (node.get() != NULL)
        event.wait();
}
//-----------------------------------------------------------------------------
long runtime_t::release(object_t* const obj) {
    assert(obj != 0);
    assert(obj->references > 0);
    // -
    bool       deleting = false;
    const long result   = atomic_decrement(&obj->references);

    if (result == 0) {
        // TN: Если ссылок на объект более нет, то только один поток имеет
        //     доступ к объекту - тот, кто осовбодил последнюю ссылку.
        //     Поэтому блокировку можно не использовать.
        if (obj->deleting) {
            // -
            if (!obj->scheduled && !obj->freeing && !obj->unimpl) {
                obj->freeing = true;
                deleting     = true;
            }
        }
        else {
            // Ссылок на объект более нет и его необходимо удалить
            obj->deleting = true;
            // -
            if (!obj->scheduled) {
                assert(obj->impl != 0);
                // -
                destroy_object_body(obj);
                // -
                obj->freeing = true;
                deleting     = true;
            }
        }
    }
    // Окончательное удаление объекта
    if (deleting)
        pushDelete(obj);
    // -
    return result;
}
//-----------------------------------------------------------------------------
// Desc:
//-----------------------------------------------------------------------------
void runtime_t::send(object_t* const target, msg_t* const msg, const TYPEID type) {
    bool undelivered = true;

    // 1. Создать пакет
    core::package_t* const package = createPackage(target, msg, type);
    // 2.
    {
        // Эксклюзивный доступ
        Exclusive   lock(target->cs);

        // Если объект отмечен для удалдения,
        // то ему более нельзя посылать сообщения
        if (!target->deleting) {
            // 1. Поставить сообщение в очередь объекта
            target->queue.push(package);
            // 2. Подобрать для него необходимый поток
            if (target->thread != 0)
                target->thread->wakeup();
            else {
                if (!target->binded && !target->scheduled) {
                    target->scheduled = true;
                    // 1. Добавить объект в очередь
                    m_queue.push(target);
                    // 2. Разбудить планировщик для обработки поступившего задания
                    m_event.signaled();
                }
            }
            undelivered = false;
        }
    }
    // 3.
    if (undelivered)
        delete package;
}
//-----------------------------------------------------------------------------
// Desc:
//-----------------------------------------------------------------------------
void runtime_t::shutdown() {
    // 1. Инициировать процедуру удаления для всех оставшихся объектов
    {
        Exclusive   lock(m_cs.actors);
        // -
        for (Actors::iterator i = m_actors.begin(); i != m_actors.end(); ++i)
            destroyObject( (*i) );
    }
    m_event.signaled();
    m_evworker.signaled();
    m_evclean.signaled();

    // 2. Дождаться, когда все объекты будут удалены
    m_evnoactors.wait();

    // 3. Дождаться, когда все потоки будут удалены
    m_terminating = true;
    // -
    m_event.signaled();
    m_evnoworkers.wait();

    // 4. Удалить системные потоки
    {
        m_active = false;
        // -
        m_event.signaled();
        m_evclean.signaled();
        // -
        m_scheduler->join();
        m_cleaner->join();
        // -
        delete m_scheduler, m_scheduler = 0;
        delete m_cleaner, m_cleaner = 0;
    }

    assert(m_workers.count == 0);
    assert(m_actors.size() == 0);
}
//-----------------------------------------------------------------------------
// Desc: Начать выполнение
//-----------------------------------------------------------------------------
void runtime_t::startup() {
    assert(m_active    == false);
    assert(m_scheduler == 0);

    // 1.
    m_active      = true;
    m_terminating = false;
    // 2.
    m_evnoactors.signaled();
    m_evnoworkers.signaled();
    // 3.
    m_cleaner   = new thread_t(MakeDelegate(this, &runtime_t::cleaner), 0);
    m_scheduler = new thread_t(MakeDelegate(this, &runtime_t::execute), 0);
}


///////////////////////////////////////////////////////////////////////////////
//                            INTERNAL METHODS                               //
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// Desc:
//-----------------------------------------------------------------------------
void runtime_t::cleaner() {
    while (m_active) {
        //
        // 1. Извлечение потоков, отмеченых для удаления
        //
        structs::localstack_t< worker_t >   queue(m_workers.deleted.extract());
        // Удалить все потоки
        while (worker_t* const item = queue.pop()) {
            delete item;
            // Уведомить, что удалены все вычислительные потоки
            if (atomic_decrement(&m_workers.count) == 0)
                m_evnoworkers.signaled();
        }
        //
        // 2.
        //
        structs::localstack_t< object_t >   objects(m_deleted.extract());
        // Удлаить заголовки объектов
        while (object_t* const item = objects.pop())
            destruct_actor(item);
        //
        // 3.
        //
        m_evclean.wait(10 * 1000);
        m_evclean.reset();
    }
}

//-----------------------------------------------------------------------------
package_t* runtime_t::createPackage(object_t* const target, msg_t* const data, const TYPEID type) {
    assert(target != 0);

    // 1. Создать экземпляр пакета
    package_t* const result = allocator_t::allocate< package_t >(data, type); //new package_t(data, type);
    // 2.
    result->sender = determineSender();
    result->target = target;
    // 3.
    acquire(result->target);
    // -
    if (result->sender)
        acquire(result->sender);
    // -
    return result;
}
//-----------------------------------------------------------------------------
worker_t* runtime_t::createWorker() {
    if (atomic_increment(&m_workers.count) > 0)
        m_evnoworkers.reset();
    // -
    try {
        worker_t::Slots     slots;
        // -
        slots.deleted = MakeDelegate(this, &runtime_t::pushDelete);
        slots.handle  = worker_t::HandlePackage(&doHandle);
        slots.idle    = MakeDelegate(this, &runtime_t::pushIdle);
        // -
        return new core::worker_t(slots);
    }
    catch (...) {
        if (atomic_decrement(&m_workers.count) == 0)
            m_evnoworkers.signaled();
        return 0;
    }
}
//-----------------------------------------------------------------------------
// Desc: Деструткор для пользовательских объектов (актеров)
//-----------------------------------------------------------------------------
void runtime_t::destruct_actor(object_t* const actor) {
    assert(actor != 0);
    assert(actor->impl == 0);
    assert(actor->references == 0);

    // -
    if (actor->thread != 0)
        atomic_decrement(&m_workers.reserved);

    // Удалить регистрацию объекта
    if (!actor->binded) {
        Exclusive   lock(m_cs.actors);
        // -
        m_actors.erase(actor);
        // -
        delete actor;
        // -
        if (m_actors.empty())
            m_evnoactors.signaled();
    }
    else
        delete actor;
}
//-----------------------------------------------------------------------------
object_t* runtime_t::determineSender() {
    return threadCtx->sender;
}
//-----------------------------------------------------------------------------
void runtime_t::execute() {
    // -
    u_int   newWorkerTimeout = 2;
    int     lastCleanupTime  = clock();

    while (m_active) {
        while(!m_queue.empty()) {
            // Прежде чем извлекать объект из очереди, необходимо проверить,
            // что есть вычислительные ресурсы для его обработки
            worker_t*   worker = m_workers.idle.pop();
            // -
            if (!worker) {
                // Если текущее количество потоков меньше оптимального,
                // то создать новый поток
                if (m_workers.count < (m_workers.reserved + (m_processors << 1)))
                    worker = createWorker();
                else {
                    // Подождать некоторое время осовобождения какого-нибудь потока
                    m_evworker.reset();
                    // -
                    const WaitResult result = m_evworker.wait(newWorkerTimeout * 1000);
                    // -
                    worker = m_workers.idle.pop();
                    // -
                    if (!worker && (result == WR_TIMEOUT)) {
                        // -
                        newWorkerTimeout += 2;
                        // -
                        if (m_workers.count < MAX_WORKERS)
                            worker = createWorker();
                        else {
                            m_evworker.reset();
                            m_evworker.wait();
                        }
                    }
                    else
                        if (newWorkerTimeout > 2)
                            newWorkerTimeout -= 2;
                }
            }
            // -
            if (worker) {
                if (object_t* const obj = m_queue.pop()) {
                    worker->assign(obj, (CLOCKS_PER_SEC >> 2));
                }
                else
                    m_workers.idle.push(worker);
            }
            yield();
        }

		// -
        if (m_terminating || (m_event.wait(10 * 1000) == WR_TIMEOUT)) {
            m_workers.deleted.push(m_workers.idle.extract());
            m_evclean.signaled();
            // -
            yield();
        }
        else {
            if ((clock() - lastCleanupTime) > (10 * CLOCKS_PER_SEC)) {
                if (worker_t* const item = m_workers.idle.pop()) {
                    m_workers.deleted.push(item);
                    m_evclean.signaled();
                }
                lastCleanupTime = clock();
            }
        }
        // -
        m_event.reset();
    }
}
//-----------------------------------------------------------------------------
void runtime_t::pushDelete(object_t* const obj) {
    assert(obj != 0);
    // -
    m_deleted.push(obj);
    // -
    m_evclean.signaled();
}
//-----------------------------------------------------------------------------
void runtime_t::pushIdle(worker_t* const worker) {
    assert(worker != 0);
    // -
    m_workers.idle.push(worker);
    // -
    m_evworker.signaled();
    m_event.signaled();
}



///////////////////////////////////////////////////////////////////////////////
//                       ИНТЕРФЕЙСНЫЕ МЕТОДЫ ЯДРА                            //
///////////////////////////////////////////////////////////////////////////////

static long counter = 0;

//-----------------------------------------------------------------------------
// Desc: Инициализация библиотеки.
//-----------------------------------------------------------------------------
void initialize() {
    if (counter == 0) {
        core::initializeThread(false);
        runtime_t::instance()->startup();
    }
    // -
    atomic_increment(&counter);
}

void initializeThread(const bool isInternal) {
    // -
    if (!threadCtx) {
        threadCtx = new thread_context_t();
        threadCtx->counter = 1;
        threadCtx->sender  = 0;
        threadCtx->is_core = isInternal;
    }
    else
        threadCtx->counter++;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void finalize() {
    if (counter > 0) {
        atomic_decrement(&counter);
        // -
        if (counter == 0) {
            finalizeThread();
            runtime_t::instance()->shutdown();
        }
    }
}

static void processActorMessages(object_t* const actor) {
    package_t* package;

    do {
        package = 0;
        {
            Exclusive   lock(actor->cs);
            // -
            if (!actor->queue.empty())
                package = actor->queue.pop();
        }

        if (package) {
            doHandle(package);
            delete package;
        }
    } while (package);
}

void finalizeThread() {
    if (threadCtx) {
        if (--threadCtx->counter == 0) {
            std::set<object_t*>::iterator   i;
            // -
            for (i = threadCtx->actors.begin(); i != threadCtx->actors.end(); ++i) {
                if (!(*i)->queue.empty())
                    processActorMessages((*i));
                
                runtime_t::instance()->destroyObject((*i));
            }
            // -
            delete threadCtx, threadCtx = 0;
        }
    }
}

void processBindedActors() {
    if (thread_t::is_core_thread())
        // Данная функция не предназначена для внутренних потоков,
        // так как они управляются ядром библиотеки
        return;
    else {
        if (threadCtx) {
            std::set<object_t*>::iterator   i;
            // -
            for (i = threadCtx->actors.begin(); i != threadCtx->actors.end(); ++i)
                processActorMessages((*i));
        }
    }
}

}; // namespace core

}; // namespace acto