/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "tcftrkgdbadapter.h"

#include "tcftrkdevice.h"
#include "trkutils.h"

#include "registerhandler.h"
#include "threadshandler.h"
#include "debuggeractions.h"
#include "debuggerstringutils.h"
#include "watchutils.h"
#ifndef STANDALONE_RUNNER
#include "gdbengine.h"
#endif

#include <utils/qtcassert.h>
#include <utils/savedaction.h>

#include <QtCore/QTimer>
#include <QtCore/QDir>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#ifdef Q_OS_WIN
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <unistd.h>
#endif

#define CB(callback) \
    static_cast<GdbEngine::AdapterCallback>(&TcfTrkGdbAdapter::callback), \
    STRINGIFY(callback)

enum { debug = 0 };

// Register names used by the 'SimpleRegister' service
static const char* tcfTrkSimpleRegisterNamesC[] =
{"R0",  "R1", "R2",  "R3",
 "R4",  "R5", "R6",  "R7",
 "R8",  "R9", "R10", "R11",
 "R12", "SP", "LR",  "PC",
 "CPSR"};

namespace Debugger {
namespace Internal {
using namespace Symbian;

static inline QString startMsg(const trk::Session &session)
{
    return TcfTrkGdbAdapter::tr("Process started, PID: 0x%1, thread id: 0x%2, "
       "code segment: 0x%3, data segment: 0x%4.")
         .arg(session.pid, 0, 16).arg(session.tid, 0, 16)
         .arg(session.codeseg, 0, 16).arg(session.dataseg, 0, 16);
}

/* -------------- TcfTrkGdbAdapter:
 * Startup-sequence:
 *  - startAdapter connects both sockets/devices
 *  - In the TCF Locator Event, gdb is started and adapterStarted is emitted.
 *  - Engine calls startInferior(), which starts the process.
 *  - Initial TCF module load suspended event is emitted (process is suspended).
 *    In the event handler, gdb is connected to the remote target. In the
 *    gdb answer to conntect remote, inferiorStartPrepared() is emitted.
 *  - Engine sets up breakpoints,etc and calls inferiorStartPhase2(), which
 *    resumes the suspended TCF process via gdb 'continue'.
 * Thread handling (30.06.2010):
 * TRK does not report thread creation/termination. So, if we receive
 * a stop in a different thread, we store an additional thread in snapshot.
 * When continuing in sendTrkContinue(), we delete this thread, since we cannot
 * know whether it will exist at the next stop.
 * Also note that threads continue running in Symbian even if one crashes.
 * TODO: - Maybe thread reporting will be improved in TCF TRK?
 *       - Stop all threads once one stops?
 *       - Breakpoints do not trigger in threads other than the main thread. */

TcfTrkGdbAdapter::TcfTrkGdbAdapter(GdbEngine *engine) :
    AbstractGdbAdapter(engine),
    m_running(false),
    m_trkDevice(new tcftrk::TcfTrkDevice(this)),
    m_gdbAckMode(true),
    m_uid(0),
    m_verbose(0),
    m_firstModuleResumableEvent(false)
{
    m_bufferedMemoryRead = true;
    // Disable buffering if gdb's dcache is used.
    m_bufferedMemoryRead = false;

    m_gdbServer = 0;
    m_gdbConnection = 0;
    m_snapshot.reset();
#ifdef Q_OS_WIN
    const DWORD portOffset = GetCurrentProcessId() % 100;
#else
    const uid_t portOffset = getuid();
#endif
    m_gdbServerName = _("127.0.0.1:%1").arg(2222 + portOffset);

    setVerbose(theDebuggerBoolSetting(VerboseLog) ? 1 : 0);

    connect(theDebuggerAction(VerboseLog), SIGNAL(valueChanged(QVariant)),
        this, SLOT(setVerbose(QVariant)));
    connect(m_trkDevice, SIGNAL(error(QString)), this, SLOT(tcftrkDeviceError(QString)));
    connect(m_trkDevice, SIGNAL(logMessage(QString)), this, SLOT(trkLogMessage(QString)));
    connect(m_trkDevice, SIGNAL(tcfEvent(tcftrk::TcfTrkEvent)), this,
            SLOT(tcftrkEvent(tcftrk::TcfTrkEvent)));

    // Set register mappings
    const int registerCount = sizeof(tcfTrkSimpleRegisterNamesC)/sizeof(const char*);
    QVector<QByteArray> registerNames;
    registerNames.reserve(registerCount);
    for (int i = 0; i < registerCount; i++)
        registerNames.push_back(QByteArray(tcfTrkSimpleRegisterNamesC[i]));
    m_trkDevice->setRegisterNames(registerNames);
}

TcfTrkGdbAdapter::~TcfTrkGdbAdapter()
{
    cleanup();
    logMessage("Shutting down.\n");
}

void TcfTrkGdbAdapter::setVerbose(const QVariant &value)
{
    setVerbose(value.toInt());
}

void TcfTrkGdbAdapter::setVerbose(int verbose)
{
    if (debug)
        qDebug("TcfTrkGdbAdapter::setVerbose %d", verbose);
    m_verbose = verbose;
    m_trkDevice->setVerbose(m_verbose);
}

void TcfTrkGdbAdapter::trkLogMessage(const QString &msg)
{
    logMessage(_("TRK ") + msg);
}

void TcfTrkGdbAdapter::setGdbServerName(const QString &name)
{
    m_gdbServerName = name;
}

QString TcfTrkGdbAdapter::gdbServerIP() const
{
    int pos = m_gdbServerName.indexOf(':');
    if (pos == -1)
        return m_gdbServerName;
    return m_gdbServerName.left(pos);
}

uint TcfTrkGdbAdapter::gdbServerPort() const
{
    int pos = m_gdbServerName.indexOf(':');
    if (pos == -1)
        return 0;
    return m_gdbServerName.mid(pos + 1).toUInt();
}

void TcfTrkGdbAdapter::handleTcfTrkRunControlModuleLoadContextSuspendedEvent(const tcftrk::TcfTrkRunControlModuleLoadContextSuspendedEvent &se)
{
    m_snapshot.resetMemory();
    const tcftrk::ModuleLoadEventInfo &minfo = se.info();
    // Register in session, keep modules and libraries in sync.
    const QString moduleName = QString::fromUtf8(minfo.name);
    if (true || minfo.loaded) { // TODO: Preliminary TCF Trk Versions always have Loaded=false?
        m_session.modules.push_back(moduleName);
        trk::Library library;
        library.name = minfo.name;
        library.codeseg = minfo.codeAddress;
        library.dataseg = minfo.dataAddress;
        library.pid = tcftrk::RunControlContext::processIdFromTcdfId(se.id());
        m_session.libraries.push_back(library);
    } else {
        const int index = m_session.modules.indexOf(moduleName);
        if (index != -1) {
            m_session.modules.removeAt(index);
            m_session.libraries.removeAt(index);
        } else {
            // Might happen with preliminary version of TCF TRK.
            qWarning("Received unload for module '%s' for which no load was received.",
                     qPrintable(moduleName));
        }

    }
    // Handle resume.
    if (se.info().requireResume) {
        // If it is the first, resumable load event (.exe), make
        // gdb connect to remote target and resume in startInferior2(),
        if (m_firstModuleResumableEvent) {
            m_firstModuleResumableEvent = false;
            m_session.codeseg = minfo.codeAddress;
            m_session.dataseg = minfo.dataAddress;
            logMessage(startMsg(m_session));
            const QByteArray symbolFile = m_symbolFile.toLocal8Bit();
            if (symbolFile.isEmpty()) {
                logMessage(_("WARNING: No symbol file available."), LogWarning);
            } else {
                // Does not seem to be necessary anymore.
                // FIXME: Startup sequence can be streamlined now as we do not
                // have to wait for the TRK startup to learn the load address.
                //m_engine->postCommand("add-symbol-file \"" + symbolFile + "\" "
                //    + QByteArray::number(m_session.codeseg));
                m_engine->postCommand("symbol-file \"" + symbolFile + "\"");
            }
            foreach(const QByteArray &s, Symbian::gdbStartupSequence())
                m_engine->postCommand(s);
            m_engine->postCommand("target remote " + gdbServerName().toLatin1(),
                                  CB(handleTargetRemote));
            if (debug)
                qDebug() << "Initial module load suspended: " << m_session.toString();
        } else {
            // Consecutive module load suspended: (not observed yet): Just continue
            m_trkDevice->sendRunControlResumeCommand(TcfTrkCallback(), se.id());
        }
    }
}

void TcfTrkGdbAdapter::handleTargetRemote(const GdbResponse &record)
{
    QTC_ASSERT(state() == InferiorStarting, qDebug() << state());
    if (record.resultClass == GdbResultDone) {
        setState(InferiorStopped);
        emit inferiorPrepared();
        if (debug)
            qDebug() << "handleTargetRemote" << m_session.toString();
    } else {
        QString msg = tr("Connecting to TRK server adapter failed:\n")
            + QString::fromLocal8Bit(record.data.findChild("msg").data());
        emit inferiorStartFailed(msg);
    }
}

void TcfTrkGdbAdapter::tcftrkEvent(const tcftrk::TcfTrkEvent &e)
{
    if (debug)
        qDebug() << e.toString() << m_session.toString() << m_snapshot.toString();
    logMessage(e.toString());

    switch (e.type()) {
    case tcftrk::TcfTrkEvent::LocatorHello:
        startGdb(); // Commands are only accepted after hello
        break;
    case tcftrk::TcfTrkEvent::RunControlModuleLoadSuspended: // A module was loaded
        handleTcfTrkRunControlModuleLoadContextSuspendedEvent(static_cast<const tcftrk::TcfTrkRunControlModuleLoadContextSuspendedEvent &>(e));
        break;
    case tcftrk::TcfTrkEvent::RunControlContextAdded: // Thread/process added
        foreach(const tcftrk::RunControlContext &rc, static_cast<const tcftrk::TcfTrkRunControlContextAddedEvent &>(e).contexts())
            if (rc.type() == tcftrk::RunControlContext::Thread)
                addThread(rc.threadId());
        break;
    case tcftrk::TcfTrkEvent::RunControlContextRemoved: // Thread/process removed
        foreach(const QByteArray &id, static_cast<const tcftrk::TcfTrkRunControlContextRemovedEvent &>(e).ids())
            switch (tcftrk::RunControlContext::typeFromTcfId(id)) {
            case tcftrk::RunControlContext::Thread:
                m_snapshot.removeThread(tcftrk::RunControlContext::threadIdFromTcdfId(id));
                break;
            case tcftrk::RunControlContext::Process:
                sendGdbServerMessage("W00", "Process exited");
                break;
        }
        break;
    case tcftrk::TcfTrkEvent::RunControlSuspended: {
            // Thread suspended/stopped
            const tcftrk::TcfTrkRunControlContextSuspendedEvent &se = static_cast<const tcftrk::TcfTrkRunControlContextSuspendedEvent &>(e);
            const unsigned threadId = tcftrk::RunControlContext::threadIdFromTcdfId(se.id());
            const QString reason = QString::fromUtf8(se.reasonID());
            showMessage(QString::fromLatin1("Reset snapshot (Thread 0x%1 stopped: '%2')").
                        arg(threadId, 0, 16).arg(reason));
            // Stopped in a new thread: Add.
            m_snapshot.reset();
            m_session.tid = threadId;
            if (m_snapshot.indexOfThread(threadId) == -1)
                m_snapshot.addThread(threadId);
            m_snapshot.setThreadState(threadId, reason);
            // Update registers first, then report stopped
            m_running = false;
            m_trkDevice->sendRegistersGetMRangeCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleAndReportReadRegistersAfterStop),
                                                       currentThreadContextId(), 0,
                                                       Symbian::RegisterCount);
    }
        break;
    default:
        break;
    }
}

void TcfTrkGdbAdapter::startGdb()
{
    QStringList gdbArgs;
    gdbArgs.append(QLatin1String("--nx")); // Do not read .gdbinit file
    if (!m_engine->startGdb(gdbArgs, QString(), QString())) {
        cleanup();
        return;
    }
    emit adapterStarted();
}

void TcfTrkGdbAdapter::tcftrkDeviceError(const QString  &errorString)
{
    logMessage(errorString);
    if (state() == EngineStarting) {
        emit adapterStartFailed(errorString, QString());
    } else {
        emit adapterCrashed(errorString);
    }
}

void TcfTrkGdbAdapter::logMessage(const QString &msg, int channel)
{
    if (m_verbose || channel != LogDebug)
        showMessage(msg, channel);
    if (debug)
        qDebug("%s", qPrintable(msg));
}

//
// Gdb
//
void TcfTrkGdbAdapter::handleGdbConnection()
{
    logMessage("HANDLING GDB CONNECTION");
    QTC_ASSERT(m_gdbConnection == 0, /**/);
    m_gdbConnection = m_gdbServer->nextPendingConnection();
    QTC_ASSERT(m_gdbConnection, return);
    connect(m_gdbConnection, SIGNAL(disconnected()),
            m_gdbConnection, SLOT(deleteLater()));
    connect(m_gdbConnection, SIGNAL(readyRead()),
            this, SLOT(readGdbServerCommand()));
}

static inline QString msgGdbPacket(const QString &p)
{
    return QLatin1String("gdb:                              ") + p;
}

void TcfTrkGdbAdapter::readGdbServerCommand()
{
    QTC_ASSERT(m_gdbConnection, return);
    QByteArray packet = m_gdbConnection->readAll();
    m_gdbReadBuffer.append(packet);

    logMessage("gdb: -> " + currentTime() + ' ' + QString::fromAscii(packet));
    if (packet != m_gdbReadBuffer)
        logMessage("buffer: " + m_gdbReadBuffer);

    QByteArray &ba = m_gdbReadBuffer;
    while (ba.size()) {
        char code = ba.at(0);
        ba = ba.mid(1);

        if (code == '+') {
            //logMessage("ACK");
            continue;
        }

        if (code == '-') {
            logMessage("NAK: Retransmission requested", LogError);
            // This seems too harsh.
            //emit adapterCrashed("Communication problem encountered.");
            continue;
        }

        if (code == char(0x03)) {
            logMessage("INTERRUPT RECEIVED");
            interruptInferior();
            continue;
        }

        if (code != '$') {
            logMessage("Broken package (2) " + quoteUnprintableLatin1(ba)
                + trk::hexNumber(code), LogError);
            continue;
        }

        int pos = ba.indexOf('#');
        if (pos == -1) {
            logMessage("Invalid checksum format in "
                + quoteUnprintableLatin1(ba), LogError);
            continue;
        }

        bool ok = false;
        uint checkSum = ba.mid(pos + 1, 2).toUInt(&ok, 16);
        if (!ok) {
            logMessage("Invalid checksum format 2 in "
                + quoteUnprintableLatin1(ba), LogError);
            return;
        }

        //logMessage(QString("Packet checksum: %1").arg(checkSum));
        trk::byte sum = 0;
        for (int i = 0; i < pos; ++i)
            sum += ba.at(i);

        if (sum != checkSum) {
            logMessage(QString("ERROR: Packet checksum wrong: %1 %2 in "
                + quoteUnprintableLatin1(ba)).arg(checkSum).arg(sum), LogError);
        }

        QByteArray cmd = ba.left(pos);
        ba.remove(0, pos + 3);
        handleGdbServerCommand(cmd);
    }
}

bool TcfTrkGdbAdapter::sendGdbServerPacket(const QByteArray &packet, bool doFlush)
{
    if (!m_gdbConnection) {
        logMessage(_("Cannot write to gdb: No connection (%1)")
            .arg(_(packet)), LogError);
        return false;
    }
    if (m_gdbConnection->state() != QAbstractSocket::ConnectedState) {
        logMessage(_("Cannot write to gdb: Not connected (%1)")
            .arg(_(packet)), LogError);
        return false;
    }
    if (m_gdbConnection->write(packet) == -1) {
        logMessage(_("Cannot write to gdb: %1 (%2)")
            .arg(m_gdbConnection->errorString()).arg(_(packet)), LogError);
        return false;
    }
    if (doFlush)
        m_gdbConnection->flush();
    return true;
}

void TcfTrkGdbAdapter::sendGdbServerAck()
{
    if (!m_gdbAckMode)
        return;
    logMessage("gdb: <- +");
    sendGdbServerPacket(QByteArray(1, '+'), false);
}

void TcfTrkGdbAdapter::sendGdbServerMessage(const QByteArray &msg, const QByteArray &logNote)
{
    trk::byte sum = 0;
    for (int i = 0; i != msg.size(); ++i)
        sum += msg.at(i);

    char checkSum[30];
    qsnprintf(checkSum, sizeof(checkSum) - 1, "%02x ", sum);

    //logMessage(QString("Packet checksum: %1").arg(sum));

    QByteArray packet;
    packet.append('$');
    packet.append(msg);
    packet.append('#');
    packet.append(checkSum);
    int pad = qMax(0, 24 - packet.size());
    logMessage("gdb: <- " + currentTime() + ' ' + packet + QByteArray(pad, ' ') + logNote);
    sendGdbServerPacket(packet, true);
}

static QByteArray msgStepRangeReceived(unsigned from, unsigned to, bool over)
{
    QByteArray rc = "Stepping range received for step ";
    rc += over ? "over" : "into";
    rc += " (0x";
    rc += QByteArray::number(from, 16);
    rc += " to 0x";
    rc += QByteArray::number(to, 16);
    rc += ')';
    return rc;
}

void TcfTrkGdbAdapter::handleGdbServerCommand(const QByteArray &cmd)
{
    if (debug)
        qDebug("handleGdbServerCommand: %s", cmd.constData());
    // http://sourceware.org/gdb/current/onlinedocs/gdb_34.html
    if (0) {}
    else if (cmd == "!") {
        sendGdbServerAck();
        //sendGdbServerMessage("", "extended mode not enabled");
        sendGdbServerMessage("OK", "extended mode enabled");
    }

    else if (cmd.startsWith('?')) {
        logMessage(msgGdbPacket(QLatin1String("Query halted")));
        // Indicate the reason the target halted.
        // The reply is the same as for step and continue.
        sendGdbServerAck();
        // The command below will trigger fetching a stack trace while
        // the process does not seem to be fully functional. Most notably
        // the PC points to a 0x9..., which is not in "our" range
        //sendGdbServerMessage("T05library:r;", "target halted (library load)");
        //sendGdbServerMessage("S05", "target halted (trap)");
        sendGdbServerMessage("S00", "target halted (trap)");
        //sendGdbServerMessage("O" + QByteArray("Starting...").toHex());
    }

    else if (cmd == "c") {
        logMessage(msgGdbPacket(QLatin1String("Continue")));
        sendGdbServerAck();
        sendTrkContinue();
    }

    else if (cmd.startsWith('C')) {
        logMessage(msgGdbPacket(QLatin1String("Continue with signal")));
        // C sig[;addr] Continue with signal sig (hex signal number)
        //Reply: See section D.3 Stop Reply Packets, for the reply specifications.
        bool ok = false;
        const uint signalNumber = cmd.mid(1).toUInt(&ok, 16);
        //TODO: Meaning of the message is not clear.
        sendGdbServerAck();
        logMessage(QString::fromLatin1("Not implemented 'Continue with signal' %1: ").arg(signalNumber), LogWarning);
        sendGdbServerMessage("O" + QByteArray("Console output").toHex());
        sendGdbServerMessage("W81"); // "Process exited with result 1
        sendTrkContinue();
    }

    else if (cmd.startsWith('D')) {
        sendGdbServerAck();
        sendGdbServerMessage("OK", "shutting down");
    }

    else if (cmd == "g") {
        // Read general registers.
        if (m_snapshot.registersValid(m_session.tid)) {
            logMessage(msgGdbPacket(QLatin1String("Read registers")));
            sendGdbServerAck();
            reportRegisters();
        } else {
            sendGdbServerAck();
            m_trkDevice->sendRegistersGetMRangeCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleAndReportReadRegisters),
                                                       currentThreadContextId(), 0,
                                                       Symbian::RegisterCount);
        }
    }

    else if (cmd == "gg") {
        // Force re-reading general registers for debugging purpose.
        sendGdbServerAck();
        m_snapshot.setRegistersValid(m_session.tid, false);
        m_trkDevice->sendRegistersGetMRangeCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleAndReportReadRegisters),
                                                   currentThreadContextId(), 0,
                                                   Symbian::RegisterCount);
    }

    else if (cmd.startsWith("salstep,")) {
        // Receive address range for current line for future use when stepping.
        sendGdbServerAck();
        m_snapshot.parseGdbStepRange(cmd, false);
        sendGdbServerMessage("", msgStepRangeReceived(m_snapshot.lineFromAddress, m_snapshot.lineToAddress, m_snapshot.stepOver));
    }

    else if (cmd.startsWith("salnext,")) {
        // Receive address range for current line for future use when stepping.
        sendGdbServerAck();
        m_snapshot.parseGdbStepRange(cmd, true);
        sendGdbServerMessage("", msgStepRangeReceived(m_snapshot.lineFromAddress, m_snapshot.lineToAddress, m_snapshot.stepOver));
    }

    else if (cmd.startsWith("Hc")) {
        sendGdbServerAck();
        gdbSetCurrentThread(cmd, "Set current thread for step & continue ");
    }

    else if (cmd.startsWith("Hg")) {
        sendGdbServerAck();
        gdbSetCurrentThread(cmd, "Set current thread ");
    }

    else if (cmd == "k" || cmd.startsWith("vKill")) {
        // Kill inferior process
        logMessage(msgGdbPacket(QLatin1String("kill")));
        // Requires id of main thread to terminate.
        // Note that calling 'Settings|set|removeExecutable' crashes TCF TRK,
        // so, it is apparently not required.
        m_trkDevice->sendRunControlTerminateCommand(TcfTrkCallback(),
                                                    mainThreadContextId());
    }

    else if (cmd.startsWith('m')) {
        logMessage(msgGdbPacket(QLatin1String("Read memory")));
        // m addr,length
        sendGdbServerAck();
        const QPair<quint64, unsigned> addrLength = parseGdbReadMemoryRequest(cmd);
        if (addrLength.first && addrLength.second) {
            readMemory(addrLength.first, addrLength.second, m_bufferedMemoryRead);
        } else {
            sendGdbServerMessage("E20", "Error " + cmd);
        }
    }

    else if (cmd.startsWith('X'))  { // Write memory
        const int dataPos = cmd.indexOf(':');
        if (dataPos == -1) {
            sendGdbServerMessage("E20", "Error (colon expected) " + cmd);
            return;
        }
        const QPair<quint64, unsigned> addrLength = parseGdbReadMemoryRequest(cmd.left(dataPos));
        if (addrLength.first == 0) {
            sendGdbServerMessage("E20", "Error (address = 0) " + cmd);
            return;
        }
        // Requests with len=0 are apparently used to probe writing.
        if (addrLength.second == 0) {
            sendGdbServerMessage("OK", "Probe memory write at 0x" + QByteArray::number(addrLength.first, 16));
            return;
        }
        // Data appear to be plain binary.
        const QByteArray data = cmd.mid(dataPos + 1);
        if (addrLength.second != unsigned(data.size())) {
            sendGdbServerMessage("E20", "Data length mismatch " + cmd);
            return;
        }
        logMessage(QString::fromLatin1("Writing %1 bytes from 0x%2: %3").
                   arg(addrLength.second).arg(addrLength.first, 0, 16).
                   arg(QString::fromAscii(data.toHex())));
        m_trkDevice->sendMemorySetCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleWriteMemory),
                                          m_tcfProcessId,
                                          addrLength.first, data);
    }

    else if (cmd.startsWith('p')) {
        logMessage(msgGdbPacket(QLatin1String("read register")));
        // 0xf == current instruction pointer?
        //sendGdbServerMessage("0000", "current IP");
        sendGdbServerAck();
        bool ok = false;
        const uint registerNumber = cmd.mid(1).toUInt(&ok, 16);
        const int threadIndex = m_snapshot.indexOfThread(m_session.tid);
        QTC_ASSERT(threadIndex != -1, return)
        const Symbian::Thread &thread =  m_snapshot.threadInfo.at(threadIndex);
        if (thread.registerValid) {
            sendGdbServerMessage(thread.gdbReportSingleRegister(registerNumber), thread.gdbSingleRegisterLogMessage(registerNumber));
        } else {
            //qDebug() << "Fetching single register";
            m_trkDevice->sendRegistersGetMRangeCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleAndReportReadRegistersAfterStop),
                                                       currentThreadContextId(), registerNumber, 1);
        }
    }

    else if (cmd.startsWith('P')) {
        logMessage(msgGdbPacket(QLatin1String("write register")));
        // $Pe=70f96678#d3
        sendGdbServerAck();
        const QPair<uint, uint> regnumValue = parseGdbWriteRegisterWriteRequest(cmd);
        // FIXME: Assume all goes well.
        m_snapshot.setRegisterValue(m_session.tid, regnumValue.first, regnumValue.second);
        logMessage(QString::fromLatin1("Setting register #%1 to 0x%2").arg(regnumValue.first).arg(regnumValue.second, 0, 16));
        m_trkDevice->sendRegistersSetCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleWriteRegister),
                                             currentThreadContextId(), regnumValue.first, regnumValue.second,
                                             QVariant(regnumValue.first));
        // Note that App TRK refuses to write registers 13 and 14
    }

    else if (cmd == "qAttached") {
        //$qAttached#8f
        // 1: attached to an existing process
        // 0: created a new process
        sendGdbServerAck();
        sendGdbServerMessage(QByteArray(1, '0'), "new process created");
    }

    else if (cmd.startsWith("qC")) {
        logMessage(msgGdbPacket(QLatin1String("query thread id")));
        // Return the current thread ID
        //$qC#b4
        sendGdbServerAck();
        sendGdbServerMessage("QC" + QByteArray::number(m_session.tid, 16));
    }

    else if (cmd.startsWith("qSupported")) {
        //$qSupported#37
        //$qSupported:multiprocess+#c6
        //logMessage("Handling 'qSupported'");
        sendGdbServerAck();
        sendGdbServerMessage(Symbian::gdbQSupported);
    }

    else if (cmd.startsWith("qThreadExtraInfo")) {
        // $qThreadExtraInfo,1f9#55
        sendGdbServerAck();
        sendGdbServerMessage(m_snapshot.gdbQThreadExtraInfo(cmd));
    }

    else if (cmd == "qfDllInfo") {
        // That's the _first_ query package.
        // Happens with  gdb 6.4.50.20060226-cvs / CodeSourcery.
        // Never made it into FSF gdb that got qXfer:libraries:read instead.
        // http://sourceware.org/ml/gdb/2007-05/msg00038.html
        // Name=hexname,TextSeg=textaddr[,DataSeg=dataaddr]
        sendGdbServerAck();
        sendGdbServerMessage(m_session.gdbQsDllInfo(), "library information transfer finished");
    }

    else if (cmd == "qsDllInfo") {
        // That's a following query package
        sendGdbServerAck();
        sendGdbServerMessage(QByteArray(1, 'l'), "library information transfer finished");
    }

    else if (cmd == "qPacketInfo") {
        // happens with  gdb 6.4.50.20060226-cvs / CodeSourcery
        // deprecated by qSupported?
        sendGdbServerAck();
        sendGdbServerMessage("", "FIXME: nothing?");
    }

    else if (cmd == "qOffsets") {
        sendGdbServerAck();
        QByteArray answer = "TextSeg=";
        answer.append(QByteArray::number(m_session.codeseg, 16));
        answer.append(";DataSeg=");
        answer.append(QByteArray::number(m_session.dataseg, 16));
        sendGdbServerMessage(answer);
    }

    else if (cmd == "qSymbol::") {
        if (m_verbose)
            logMessage(msgGdbPacket(QLatin1String("notify can handle symbol lookup")));
        // Notify the target that GDB is prepared to serve symbol lookup requests.
        sendGdbServerAck();
        if (1)
            sendGdbServerMessage("OK", "no further symbols needed");
        else
            sendGdbServerMessage("qSymbol:" + QByteArray("_Z7E32Mainv").toHex(),
                "ask for more");
    }

    else if (cmd.startsWith("qXfer:features:read:target.xml:")) {
        //  $qXfer:features:read:target.xml:0,7ca#46...Ack
        sendGdbServerAck();
        sendGdbServerMessage(Symbian::gdbArchitectureXml);
    }

    else if (cmd == "qfThreadInfo") {
        // That's the _first_ query package.
        sendGdbServerAck();
        sendGdbServerMessage(m_snapshot.gdbQsThreadInfo(), "thread information transferred");
    }

    else if (cmd == "qsThreadInfo") {
        // That's a following query package
        sendGdbServerAck();
        sendGdbServerMessage(QByteArray(1, 'l'), "thread information transfer finished");
    }

    else if (cmd.startsWith("qXfer:libraries:read")) {
        sendGdbServerAck();
        sendGdbServerMessage(m_session.gdbLibraryList(), "library information transferred");
    }

    else if (cmd == "QStartNoAckMode") {
        //$qSupported#37
        logMessage("Handling 'QStartNoAckMode'");
        sendGdbServerAck();
        sendGdbServerMessage("OK", "ack no-ack mode");
        m_gdbAckMode = false;
    }

    else if (cmd.startsWith("QPassSignals")) {
        // list of signals to pass directly to inferior
        // $QPassSignals:e;10;14;17;1a;1b;1c;21;24;25;4c;#8f
        // happens only if "QPassSignals+;" is qSupported
        sendGdbServerAck();
        // FIXME: use the parameters
        sendGdbServerMessage("OK", "passing signals accepted");
    }

    else if (cmd == "s" || cmd.startsWith("vCont;s")) {
        const uint pc = m_snapshot.registerValue(m_session.tid, RegisterPC);
        logMessage(msgGdbPacket(QString::fromLatin1("Step range from 0x%1").
                                arg(pc, 0, 16)));
        sendGdbServerAck();
        m_running = true;
        sendTrkStepRange();
    }

    else if (cmd.startsWith('T')) {
        // FIXME: check whether thread is alive
        sendGdbServerAck();
        sendGdbServerMessage("OK"); // pretend all is well
    }

    else if (cmd == "vCont?") {
        // actions supported by the vCont packet
        sendGdbServerAck();
        sendGdbServerMessage("vCont;c;C;s;S"); // we don't support vCont.
    }

    else if (cmd == "vCont;c") {
        // vCont[;action[:thread-id]]...'
        sendGdbServerAck();
        m_running = true;
        sendTrkContinue();
    }

    else if (cmd.startsWith("Z0,") || cmd.startsWith("Z1,")) {
        // Insert breakpoint
        sendGdbServerAck();
        logMessage(msgGdbPacket(QLatin1String("Insert breakpoint")));
        // $Z0,786a4ccc,4#99
        const QPair<quint64, unsigned> addrLen = parseGdbSetBreakpointRequest(cmd);
        if (addrLen.first) {
            //qDebug() << "ADDR: " << hexNumber(addr) << " LEN: " << len;
            logMessage(_("Inserting breakpoint at 0x%1, %2")
                .arg(addrLen.first, 0, 16).arg(addrLen.second));
            // const QByteArray ba = trkBreakpointMessage(addr, len, len == 4);
            tcftrk::Breakpoint bp(addrLen.first);
            bp.size = addrLen.second;
            bp.setContextId(m_session.pid);
            // We use the automatic ids calculated from the location
            // address instead of the map in snapshot.
            m_trkDevice->sendBreakpointsAddCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleAndReportSetBreakpoint),
                                                   bp);
        } else {
            logMessage("MISPARSED BREAKPOINT '" + cmd + "'')" , LogError);
        }
    }

    else if (cmd.startsWith("z0,") || cmd.startsWith("z1,")) {
        // Remove breakpoint
        sendGdbServerAck();
        logMessage(msgGdbPacket(QLatin1String("Remove breakpoint")));
        // $z0,786a4ccc,4#99
        const int pos = cmd.lastIndexOf(',');
        const uint addr = cmd.mid(3, pos - 3).toUInt(0, 16);
        m_trkDevice->sendBreakpointsRemoveCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleClearBreakpoint),
                                                  tcftrk::Breakpoint::idFromLocation(addr));
    }

    else if (cmd.startsWith("qPart:") || cmd.startsWith("qXfer:"))  {
        QByteArray data = cmd.mid(1 + cmd.indexOf(':'));
        // "qPart:auxv:read::0,147": Read OS auxiliary data (see info aux)
        bool handled = false;
        if (data.startsWith("auxv:read::")) {
            const int offsetPos = data.lastIndexOf(':') + 1;
            const int commaPos = data.lastIndexOf(',');
            if (commaPos != -1) {
                bool ok1 = false, ok2 = false;
                const int offset = data.mid(offsetPos,  commaPos - offsetPos)
                    .toUInt(&ok1, 16);
                const int length = data.mid(commaPos + 1).toUInt(&ok2, 16);
                if (ok1 && ok2) {
                    const QString msg = _("Read of OS auxiliary "
                        "vector (%1, %2) not implemented.").arg(offset).arg(length);
                    logMessage(msgGdbPacket(msg), LogWarning);
                    sendGdbServerMessage("E20", msg.toLatin1());
                    handled = true;
                }
            }
        } // auxv read

        if (!handled) {
            const QString msg = QLatin1String("FIXME unknown 'XFER'-request: ")
                + QString::fromAscii(cmd);
            logMessage(msgGdbPacket(msg), LogWarning);
            sendGdbServerMessage("E20", msg.toLatin1());
        }

    } // qPart/qXfer
       else {
        logMessage(msgGdbPacket(QLatin1String("FIXME unknown: ")
            + QString::fromAscii(cmd)), LogWarning);
    }
}

void TcfTrkGdbAdapter::gdbSetCurrentThread(const QByteArray &cmd, const char *why)
{
    // Thread ID from Hg/Hc commands: '-1': All, '0': arbitrary, else hex thread id.
    const QByteArray id = cmd.mid(2);
    const int threadId = id == "-1" ? -1 : id.toInt(0, 16);
    const QByteArray message = QByteArray(why) + QByteArray::number(threadId);
    logMessage(msgGdbPacket(QString::fromLatin1(message)));
    // Set thread for subsequent operations (`m', `M', `g', `G', et.al.).
    // for 'other operations.  0 - any thread
    //$Hg0#df
    m_session.tid = threadId <= 0 ? m_session.mainTid : uint(threadId);
    sendGdbServerMessage("OK", message);
}

void TcfTrkGdbAdapter::interruptInferior()
{
    m_trkDevice->sendRunControlSuspendCommand(TcfTrkCallback(), m_tcfProcessId);
}

void TcfTrkGdbAdapter::startAdapter()
{
    const ushort tcfTrkPort = 1534;

    m_snapshot.fullReset();
    m_session.reset();
    m_firstModuleResumableEvent = true;
    m_tcfProcessId.clear();

    // Retrieve parameters
    const DebuggerStartParameters &parameters = startParameters();
    m_remoteExecutable = parameters.executable;
    m_remoteArguments = parameters.processArgs;
    m_symbolFile = parameters.symbolFileName;

    QString tcfTrkAddress;
    QSharedPointer<QTcpSocket> tcfTrkSocket(new QTcpSocket);
    m_trkDevice->setDevice(tcfTrkSocket);
    m_trkIODevice = tcfTrkSocket;

    if (debug)
        qDebug() << parameters.processArgs;
    // Fixme: 1 of 3 testing hacks.
    if (parameters.processArgs.size() >= 5 && parameters.processArgs.at(0) == _("@tcf@")) {
        m_remoteExecutable = parameters.processArgs.at(1);
        m_uid = parameters.processArgs.at(2).toUInt(0, 16);
        m_symbolFile = parameters.processArgs.at(3);
        tcfTrkAddress = parameters.processArgs.at(4);
        m_remoteArguments.clear();
    } else {
        emit adapterStartFailed(_("Parameter error"), QString());
    }

    // Unixish gdbs accept only forward slashes
    m_symbolFile.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // Start
    QTC_ASSERT(state() == EngineStarting, qDebug() << state());
    showMessage(_("TRYING TO START ADAPTER"));
    logMessage(QLatin1String("### Starting TcfTrkGdbAdapter"));

    QTC_ASSERT(m_gdbServer == 0, delete m_gdbServer);
    QTC_ASSERT(m_gdbConnection == 0, m_gdbConnection = 0);
    m_gdbServer = new QTcpServer(this);

    if (!m_gdbServer->listen(QHostAddress(gdbServerIP()), gdbServerPort())) {
        QString msg = QString("Unable to start the gdb server at %1: %2.")
            .arg(m_gdbServerName).arg(m_gdbServer->errorString());
        logMessage(msg, LogError);
        emit adapterStartFailed(msg, QString());
        return;
    }

    logMessage(QString("Gdb server running on %1.\nLittle endian assumed.")
        .arg(m_gdbServerName));

    connect(m_gdbServer, SIGNAL(newConnection()),
        this, SLOT(handleGdbConnection()));

    logMessage(QString::fromLatin1("Connecting to TCF TRK on %1:%2")
               .arg(tcfTrkAddress).arg(tcfTrkPort));
    tcfTrkSocket->connectToHost(tcfTrkAddress, tcfTrkPort);
}

void TcfTrkGdbAdapter::startInferior()
{
    QTC_ASSERT(state() == InferiorStarting, qDebug() << state());
    m_trkDevice->sendProcessStartCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleCreateProcess),
                                         m_remoteExecutable, m_uid, m_remoteArguments,
                                         QString(), true);

}

void TcfTrkGdbAdapter::addThread(unsigned id)
{
    // Make thread known, register as main if it is the first one.
    if (m_snapshot.indexOfThread(id) == -1) {
        m_snapshot.addThread(id);
        if (m_session.tid == 0) {
            m_session.tid = id;
            if (m_session.mainTid == 0)
                m_session.mainTid = id;
        }
    }
}

void TcfTrkGdbAdapter::handleCreateProcess(const tcftrk::TcfTrkCommandResult &result)
{
    if (debug)
        qDebug() << "ProcessCreated: " << result.toString();
    if (!result) {
        const QString errorMessage = result.errorString();
        logMessage(QString::fromLatin1("Failed to start process: %1").arg(errorMessage), LogError);
        emit inferiorStartFailed(result.errorString());
        return;
    }
    QTC_ASSERT(!result.values.isEmpty(), return);

    tcftrk::RunControlContext ctx;
    ctx.parse(result.values.front());
    logMessage(ctx.toString());

    m_session.pid = ctx.processId();
    m_tcfProcessId = tcftrk::RunControlContext::tcfId(m_session.pid);
    if (const unsigned threadId = ctx.threadId())
        addThread(threadId);
    // See ModuleLoadSuspendedEvent for the rest.
    m_session.codeseg = 0;
    m_session.dataseg = 0;
}

void TcfTrkGdbAdapter::startInferiorPhase2()
{
    m_engine->continueInferiorInternal();
}

//
// AbstractGdbAdapter interface implementation
//

void TcfTrkGdbAdapter::write(const QByteArray &data)
{
    // Write magic packets directly to TRK.
    if (data.startsWith("@#")) {
        QByteArray data1 = data.mid(2);
        if (data1.endsWith(char(10)))
            data1.chop(1);
        if (data1.endsWith(char(13)))
            data1.chop(1);
        if (data1.endsWith(' '))
            data1.chop(1);
        bool ok;
        const uint addr = data1.toUInt(&ok, 0);
        logMessage(QString::fromLatin1("Direct step (@#) 0x%1: not implemented").arg(addr, 0, 16), LogError);
        // directStep(addr);
        return;
    }
    if (data.startsWith("@$")) {
        QByteArray ba = QByteArray::fromHex(data.mid(2));
        qDebug() << "Writing: " << quoteUnprintableLatin1(ba);
        // if (ba.size() >= 1)
        // sendTrkMessage(ba.at(0), TrkCB(handleDirectTrk), ba.mid(1));
        return;
    }
    if (data.startsWith("@@")) {
        logMessage(QLatin1String("Direct write (@@): not implemented"), LogError);
        return;
    }
    m_gdbProc.write(data);
}

void TcfTrkGdbAdapter::cleanup()
{
    delete m_gdbServer;
    m_gdbServer = 0;
    if (!m_trkIODevice.isNull()) {
                QAbstractSocket *socket = qobject_cast<QAbstractSocket *>(m_trkIODevice.data());
        const bool isOpen = socket ? socket->state() == QAbstractSocket::ConnectedState : m_trkIODevice->isOpen();
        if (isOpen) {
            if (socket) {
                socket->disconnect();
            } else {
                m_trkIODevice->close();
            }
        }
    } //!m_trkIODevice.isNull()
}

void TcfTrkGdbAdapter::shutdown()
{
    cleanup();
}

void TcfTrkGdbAdapter::trkReloadRegisters()
{
    // Take advantage of direct access to cached register values.
    m_snapshot.syncRegisters(m_session.tid, m_engine->registerHandler());
}

void TcfTrkGdbAdapter::trkReloadThreads()
{
    m_snapshot.syncThreads(m_engine->threadsHandler());
}

void TcfTrkGdbAdapter::handleWriteRegister(const tcftrk::TcfTrkCommandResult &result)
{
    const int registerNumber = result.cookie.toInt();
    if (result) {
        sendGdbServerMessage("OK");
    } else {
        logMessage(QString::fromLatin1("ERROR writing register #%1: %2").arg(registerNumber).arg(result.errorString()), LogError);
        sendGdbServerMessage("E01");
    }
}

void TcfTrkGdbAdapter::reportRegisters()
{
    const int threadIndex = m_snapshot.indexOfThread(m_session.tid);
    QTC_ASSERT(threadIndex != -1, return);
    const Symbian::Thread &thread = m_snapshot.threadInfo.at(threadIndex);
    sendGdbServerMessage(thread.gdbReportRegisters(), thread.gdbRegisterLogMessage(m_verbose));
}

void TcfTrkGdbAdapter::handleReadRegisters(const tcftrk::TcfTrkCommandResult &result)
{
    logMessage("       REGISTER RESULT: " + result.toString());
    if (!result) {
        logMessage("ERROR: " + result.errorString(), LogError);
        return;
    }
    if (result.values.isEmpty() || result.values.front().type() != tcftrk::JsonValue::Array) {
        logMessage(_("Format error in register message: ") + result.toString(), LogError);
        return;
    }
    unsigned i = result.cookie.toUInt();
    uint *registers = m_snapshot.registers(m_session.tid);
    QTC_ASSERT(registers, return;)
    foreach (const tcftrk::JsonValue &jr, result.values.front().children())
        registers[i++] = jr.data().toUInt(0, 16);
    m_snapshot.setRegistersValid(m_session.tid, true);
    if (debug)
        qDebug() << "handleReadRegisters: " << m_snapshot.toString();
}

void TcfTrkGdbAdapter::handleAndReportReadRegisters(const tcftrk::TcfTrkCommandResult &result)
{
    handleReadRegisters(result);
    reportRegisters();
}

void TcfTrkGdbAdapter::handleAndReportReadRegister(const tcftrk::TcfTrkCommandResult &result)
{
    handleReadRegisters(result);
    const uint registerNumber = result.cookie.toUInt();
    const int threadIndex = m_snapshot.indexOfThread(m_session.tid);
    QTC_ASSERT(threadIndex != -1, return);
    const Symbian::Thread &thread = m_snapshot.threadInfo.at(threadIndex);
    sendGdbServerMessage(thread.gdbReportSingleRegister(registerNumber), thread.gdbSingleRegisterLogMessage(registerNumber));
}

void TcfTrkGdbAdapter::handleAndReportReadRegistersAfterStop(const tcftrk::TcfTrkCommandResult &result)
{
    handleReadRegisters(result);
    handleReadRegisters(result);
    const bool reportThread = m_session.tid != m_session.mainTid;
    sendGdbServerMessage(m_snapshot.gdbStopMessage(m_session.tid, reportThread),
                         "Stopped with registers in thread " + QByteArray::number(m_session.tid, 16));
}

void TcfTrkGdbAdapter::handleAndReportSetBreakpoint(const tcftrk::TcfTrkCommandResult &result)
{
    if (result) {
        sendGdbServerMessage("OK");
    } else {
        logMessage(QLatin1String("Error setting breakpoint: ") + result.errorString(), LogError);
        sendGdbServerMessage("E21");
    }
}

void TcfTrkGdbAdapter::handleClearBreakpoint(const tcftrk::TcfTrkCommandResult &result)
{
    logMessage("CLEAR BREAKPOINT ");
    if (!result)
        logMessage("Error clearing breakpoint: " + result.errorString(), LogError);
    sendGdbServerMessage("OK");
}

void TcfTrkGdbAdapter::readMemory(uint addr, uint len, bool buffered)
{
    Q_ASSERT(len < (2 << 16));

    // We try to get medium-sized chunks of data from the device
    if (m_verbose > 2)
        logMessage(_("readMemory %1 bytes from 0x%2 blocksize=%3")
            .arg(len).arg(addr, 0, 16).arg(MemoryChunkSize));

    m_snapshot.wantedMemory = MemoryRange(addr, addr + len);
    tryAnswerGdbMemoryRequest(buffered);
}

static QString msgMemoryReadError(uint addr, uint len = 0)
{
    const QString lenS = len ? QString::number(len) : QLatin1String("<unknown>");
    return _("Memory read error at: 0x%1 %2").arg(addr, 0, 16).arg(lenS);
}

void TcfTrkGdbAdapter::sendMemoryGetCommand(const MemoryRange &range, bool buffered)
{
    const QVariant cookie = QVariant::fromValue(range);
    const TcfTrkCallback cb = buffered ?
                              TcfTrkCallback(this, &TcfTrkGdbAdapter::handleReadMemoryBuffered) :
                              TcfTrkCallback(this, &TcfTrkGdbAdapter::handleReadMemoryUnbuffered);
    m_trkDevice->sendMemoryGetCommand(cb, currentThreadContextId(), range.from, range.size(), cookie);
}

void TcfTrkGdbAdapter::handleReadMemoryBuffered(const tcftrk::TcfTrkCommandResult &result)
{
    QTC_ASSERT(qVariantCanConvert<MemoryRange>(result.cookie), return);

    const QByteArray memory = tcftrk::TcfTrkDevice::parseMemoryGet(result);
    const MemoryRange range = result.cookie.value<MemoryRange>();

    if (unsigned(memory.size()) != range.size()) {
        logMessage(_("TEMPORARY: ") + msgMemoryReadError(range.from, range.size()));
        logMessage(_("RETRYING UNBUFFERED"));
        // FIXME: This does not handle large requests properly.
        sendMemoryGetCommand(range, false);
        return;
    }
    m_snapshot.insertMemory(range, memory);
    tryAnswerGdbMemoryRequest(true);
}

void TcfTrkGdbAdapter::handleReadMemoryUnbuffered(const tcftrk::TcfTrkCommandResult &result)
{
    QTC_ASSERT(qVariantCanConvert<MemoryRange>(result.cookie), return);

    const QByteArray memory = tcftrk::TcfTrkDevice::parseMemoryGet(result);
    const MemoryRange range = result.cookie.value<MemoryRange>();

    if (unsigned(memory.size()) != range.size()) {
        logMessage(_("TEMPORARY: ") + msgMemoryReadError(range.from, range.size()));
        logMessage(_("RETRYING UNBUFFERED"));
        const QByteArray ba = "E20";
        sendGdbServerMessage(ba, msgMemoryReadError(32, range.from).toLatin1());
        return;
    }
    m_snapshot.insertMemory(range, memory);
    tryAnswerGdbMemoryRequest(false);
}

void TcfTrkGdbAdapter::tryAnswerGdbMemoryRequest(bool buffered)
{
    //logMessage("TRYING TO ANSWER MEMORY REQUEST ");
    MemoryRange wanted = m_snapshot.wantedMemory;
    MemoryRange needed = m_snapshot.wantedMemory;
    MEMORY_DEBUG("WANTED: " << wanted);
    Snapshot::Memory::const_iterator it = m_snapshot.memory.begin();
    Snapshot::Memory::const_iterator et = m_snapshot.memory.end();
    for ( ; it != et; ++it) {
        MEMORY_DEBUG("   NEEDED STEP: " << needed);
        needed -= it.key();
    }
    MEMORY_DEBUG("NEEDED FINAL: " << needed);

    if (needed.to == 0) {
        // FIXME: need to combine chunks first.

        // All fine. Send package to gdb.
        it = m_snapshot.memory.begin();
        et = m_snapshot.memory.end();
        for ( ; it != et; ++it) {
            if (it.key().from <= wanted.from && wanted.to <= it.key().to) {
                int offset = wanted.from - it.key().from;
                int len = wanted.to - wanted.from;
                QByteArray ba = it.value().mid(offset, len);
                sendGdbServerMessage(ba.toHex(),
                                     m_snapshot.memoryReadLogMessage(wanted.from, m_session.tid, m_verbose, ba));
                return;
            }
        }
        // Happens when chunks are not combined
        QTC_ASSERT(false, /**/);
        showMessage("CHUNKS NOT COMBINED");
#        ifdef MEMORY_DEBUG
        qDebug() << "CHUNKS NOT COMBINED";
        it = m_snapshot.memory.begin();
        et = m_snapshot.memory.end();
        for ( ; it != et; ++it)
            qDebug() << trk::hexNumber(it.key().from) << trk::hexNumber(it.key().to);
        qDebug() << "WANTED" << wanted.from << wanted.to;
#        endif
        sendGdbServerMessage("E22", "");
        return;
    }

    MEMORY_DEBUG("NEEDED AND UNSATISFIED: " << needed);
    if (buffered) {
        uint blockaddr = (needed.from / MemoryChunkSize) * MemoryChunkSize;
        logMessage(_("Requesting buffered memory %1 bytes from 0x%2")
            .arg(MemoryChunkSize).arg(blockaddr, 0, 16));
        MemoryRange range(blockaddr, blockaddr + MemoryChunkSize);
        MEMORY_DEBUG("   FETCH BUFFERED MEMORY : " << range);
        sendMemoryGetCommand(range, true);
    } else { // Unbuffered, direct requests
        int len = needed.to - needed.from;
        logMessage(_("Requesting unbuffered memory %1 bytes from 0x%2")
            .arg(len).arg(needed.from, 0, 16));
        sendMemoryGetCommand(needed, false);
        MEMORY_DEBUG("   FETCH UNBUFFERED MEMORY : " << needed);
    }
}

void TcfTrkGdbAdapter::handleWriteMemory(const tcftrk::TcfTrkCommandResult &result)
{
    if (result) {
        sendGdbServerMessage("OK", "Write memory");
    } else {
        logMessage(QLatin1String("Error writing memory: ") + result.errorString(), LogError);
        sendGdbServerMessage("E21");
    }
}

QByteArray TcfTrkGdbAdapter::mainThreadContextId() const
{
    return tcftrk::RunControlContext::tcfId(m_session.pid, m_session.mainTid);
}

QByteArray TcfTrkGdbAdapter::currentThreadContextId() const
{
    return tcftrk::RunControlContext::tcfId(m_session.pid, m_session.tid);
}

void TcfTrkGdbAdapter::sendTrkContinue()
{
    // Remove all but main thread as we do not know whether they will exist
    // at the next stop.
    if (m_snapshot.threadInfo.size() > 1)
        m_snapshot.threadInfo.remove(1, m_snapshot.threadInfo.size() - 1);
    m_trkDevice->sendRunControlResumeCommand(TcfTrkCallback(), m_tcfProcessId);
}

void TcfTrkGdbAdapter::sendTrkStepRange()
{
    uint from = m_snapshot.lineFromAddress;
    uint to = m_snapshot.lineToAddress;
    const uint pc = m_snapshot.registerValue(m_session.tid, RegisterPC);
    if (from <= pc && pc <= to) {
        const QString msg = QString::fromLatin1("Step in 0x%1 .. 0x%2 instead of 0x%3...").
                            arg(from, 0, 16).arg(to, 0, 16).arg(pc, 0, 16);
        showMessage(msg);
    } else {
        from = pc;
        to = pc;
    }
    // TODO: Step range does not seem to work yet?
    const tcftrk::RunControlResumeMode mode = (from == to && to == pc) ?
          (m_snapshot.stepOver ? tcftrk::RM_STEP_OVER       : tcftrk::RM_STEP_INTO) :
          (m_snapshot.stepOver ? tcftrk::RM_STEP_OVER_RANGE : tcftrk::RM_STEP_INTO_RANGE);

    logMessage(QString::fromLatin1("Stepping from 0x%1 to 0x%2 (current PC=0x%3), mode %4").
               arg(from, 0, 16).arg(to, 0, 16).arg(pc).arg(int(mode)));
    m_trkDevice->sendRunControlResumeCommand(TcfTrkCallback(this, &TcfTrkGdbAdapter::handleStep),
                                             currentThreadContextId(),
                                             mode, 1, from, to);
}

void TcfTrkGdbAdapter::handleStep(const tcftrk::TcfTrkCommandResult &result)
{

    if (!result) { // Try fallback with Continue.
        logMessage(QString::fromLatin1("Error while stepping: %1 (fallback to 'continue')").arg(result.errorString()), LogWarning);
        sendTrkContinue();
        // Doing nothing as below does not work as gdb seems to insist on
        // making some progress through a 'step'.
        //sendTrkMessage(0x12,
        //    TrkCB(handleAndReportReadRegistersAfterStop),
        //    trkReadRegistersMessage());
        return;
    }
    // The gdb server response is triggered later by the Stop Reply packet.
    logMessage("STEP FINISHED " + currentTime());
}


} // namespace Internal
} // namespace Debugger
