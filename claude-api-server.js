#!/usr/bin/env node

/**
 * VoodooPS2 ETD0180 Calibration - Claude API Server
 * 
 * Provides live API endpoints for Claude to monitor and interact with 
 * the calibration process in real-time.
 * 
 * Usage: node claude-api-server.js
 * API: http://localhost:8080
 */

const http = require('http');
const url = require('url');
const { spawn } = require('child_process');

const PORT = 8080;
const HOST = 'localhost';

// Global state storage
let calibrationState = {
    connected: false,
    capturing: false,
    startTime: null,
    logs: [],
    packets: {
        total: 0,
        movement: 0,
        buttons: 0
    },
    currentPosition: { x: 1000, y: 1000 },
    settings: {
        gain: 3.0,
        sensitivity: 1.0,
        maxX: 2080,
        maxY: 2412
    },
    realTimeData: {
        lastPacket: null,
        lastDelta: { dx: 0, dy: 0 },
        lastButtons: { left: 0, right: 0, middle: 0 },
        heatMap: []
    }
};

// Live log streaming process
let logProcess = null;

class VoodooPS2LogProcessor {
    constructor() {
        this.logBuffer = [];
        this.isProcessing = false;
    }
    
    startCapture() {
        if (this.isProcessing) {
            console.log('⚠️  Log capture already running');
            return false;
        }
        
        console.log('🚀 Starting live ETD0180 log capture...');
        
        // Start macOS log streaming for ELAN_CALIB messages
        logProcess = spawn('log', [
            'stream',
            '--predicate', 'message CONTAINS "ELAN_CALIB"',
            '--style', 'compact',
            '--color', 'never'
        ]);
        
        logProcess.stdout.on('data', (data) => {
            const lines = data.toString().split('\n');
            lines.forEach(line => this.processLogLine(line.trim()));
        });
        
        logProcess.stderr.on('data', (data) => {
            console.error(`Log capture error: ${data}`);
        });
        
        logProcess.on('close', (code) => {
            console.log(`📊 Log capture ended with code ${code}`);
            this.isProcessing = false;
            calibrationState.capturing = false;
        });
        
        this.isProcessing = true;
        calibrationState.capturing = true;
        calibrationState.startTime = new Date().toISOString();
        
        return true;
    }
    
    stopCapture() {
        if (logProcess) {
            console.log('⏹️  Stopping log capture...');
            logProcess.kill('SIGTERM');
            logProcess = null;
        }
        this.isProcessing = false;
        calibrationState.capturing = false;
    }
    
    processLogLine(line) {
        if (!line || line.length === 0) return;
        
        // Add to log buffer (keep last 200 entries)
        calibrationState.logs.push({
            timestamp: new Date().toISOString(),
            message: line,
            type: this.classifyLogType(line)
        });
        
        if (calibrationState.logs.length > 200) {
            calibrationState.logs.shift();
        }
        
        // Parse specific ETD0180 log patterns
        this.parseETD0180Data(line);
    }
    
    classifyLogType(line) {
        if (line.includes('ETD0180_PACKET')) return 'packet';
        if (line.includes('ETD0180_DELTAS')) return 'delta';
        if (line.includes('ETD0180_STATE')) return 'state';
        if (line.includes('ETD0180_OUTPUT')) return 'output';
        if (line.includes('ETD0180_LIFT')) return 'lift';
        if (line.includes('sanity check failed')) return 'error';
        return 'info';
    }
    
    parseETD0180Data(line) {
        try {
            // Parse packet data
            if (line.includes('ETD0180_PACKET')) {
                calibrationState.packets.total++;
                const packetMatch = line.match(/\[0\]=0x([0-9a-f]{2}).*\[1\]=0x([0-9a-f]{2}).*\[2\]=0x([0-9a-f]{2})/i);
                if (packetMatch) {
                    calibrationState.realTimeData.lastPacket = {
                        byte0: parseInt(packetMatch[1], 16),
                        byte1: parseInt(packetMatch[2], 16),
                        byte2: parseInt(packetMatch[3], 16),
                        timestamp: Date.now()
                    };
                }
            }
            
            // Parse delta data
            if (line.includes('ETD0180_DELTAS')) {
                calibrationState.packets.movement++;
                const deltaMatch = line.match(/dx=(-?\d+) dy=(-?\d+).*buttons=L(\d+),R(\d+),M(\d+)/);
                if (deltaMatch) {
                    const dx = parseInt(deltaMatch[1]);
                    const dy = parseInt(deltaMatch[2]);
                    const leftBtn = parseInt(deltaMatch[3]);
                    const rightBtn = parseInt(deltaMatch[4]);
                    const middleBtn = parseInt(deltaMatch[5]);
                    
                    calibrationState.realTimeData.lastDelta = { dx, dy };
                    calibrationState.realTimeData.lastButtons = { 
                        left: leftBtn, 
                        right: rightBtn, 
                        middle: middleBtn 
                    };
                    
                    if (leftBtn || rightBtn || middleBtn) {
                        calibrationState.packets.buttons++;
                    }
                    
                    // Update position (simulate integrator)
                    calibrationState.currentPosition.x += dx * calibrationState.settings.gain;
                    calibrationState.currentPosition.y += dy * calibrationState.settings.gain;
                    
                    // Clamp position
                    calibrationState.currentPosition.x = Math.max(0, 
                        Math.min(calibrationState.settings.maxX - 1, calibrationState.currentPosition.x));
                    calibrationState.currentPosition.y = Math.max(0, 
                        Math.min(calibrationState.settings.maxY - 1, calibrationState.currentPosition.y));
                    
                    // Add to heat map
                    calibrationState.realTimeData.heatMap.push({
                        x: calibrationState.currentPosition.x,
                        y: calibrationState.currentPosition.y,
                        timestamp: Date.now()
                    });
                    
                    // Keep heat map size reasonable
                    if (calibrationState.realTimeData.heatMap.length > 1000) {
                        calibrationState.realTimeData.heatMap.shift();
                    }
                }
            }
            
        } catch (error) {
            console.error('Error parsing ETD0180 data:', error);
        }
    }
}

// Initialize log processor
const logProcessor = new VoodooPS2LogProcessor();

// HTTP Server with API endpoints
const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    const path = parsedUrl.pathname;
    const query = parsedUrl.query;
    
    // CORS headers for browser access
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
    
    if (req.method === 'OPTIONS') {
        res.writeHead(200);
        res.end();
        return;
    }
    
    // Route requests
    try {
        switch (path) {
            case '/':
                handleRoot(req, res);
                break;
            case '/api/status':
                handleStatus(req, res);
                break;
            case '/api/connect':
                handleConnect(req, res);
                break;
            case '/api/start-capture':
                handleStartCapture(req, res);
                break;
            case '/api/stop-capture':
                handleStopCapture(req, res);
                break;
            case '/api/logs':
                handleLogs(req, res, query);
                break;
            case '/api/calibration':
                handleCalibration(req, res);
                break;
            case '/api/settings':
                handleSettings(req, res);
                break;
            case '/api/metrics':
                handleMetrics(req, res);
                break;
            case '/api/realtime':
                handleRealtime(req, res);
                break;
            case '/api/test-result':
                if (req.method === 'POST') {
                    handleTestResult(req, res);
                } else {
                    handle404(req, res);
                }
                break;
            default:
                handle404(req, res);
        }
    } catch (error) {
        console.error('API Error:', error);
        res.writeHead(500, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Internal server error', message: error.message }));
    }
});

// API Handlers
function handleRoot(req, res) {
    res.writeHead(200, { 'Content-Type': 'text/html' });
    res.end(`
        <h1>🔬 VoodooPS2 ETD0180 Claude API Server</h1>
        <p><strong>Status:</strong> Running on http://${HOST}:${PORT}</p>
        <p><strong>Calibration Tool:</strong> <a href="file:///Volumes/INTENSO/VoodooPS2Elan/calibration-tool.html">Open Tool</a></p>
        <h2>Available Endpoints:</h2>
        <ul>
            <li><code>GET /api/status</code> - Connection and capture status</li>
            <li><code>POST /api/start-capture</code> - Start log capture</li>
            <li><code>POST /api/stop-capture</code> - Stop log capture</li>
            <li><code>GET /api/logs?limit=N</code> - Recent logs (default: 20)</li>
            <li><code>GET /api/calibration</code> - Complete calibration data</li>
            <li><code>GET /api/metrics</code> - Packet statistics</li>
            <li><code>GET /api/realtime</code> - Live trackpad data</li>
            <li><code>POST /api/settings</code> - Update calibration settings</li>
        </ul>
        <p><em>This server enables Claude to monitor ETD0180 calibration in real-time.</em></p>
    `);
}

function handleStatus(req, res) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        connected: calibrationState.connected,
        capturing: calibrationState.capturing,
        startTime: calibrationState.startTime,
        uptime: calibrationState.startTime ? Date.now() - new Date(calibrationState.startTime).getTime() : 0,
        logCount: calibrationState.logs.length,
        server: 'VoodooPS2-ETD0180-API',
        version: '1.0.0'
    }));
}

function handleConnect(req, res) {
    calibrationState.connected = true;
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ 
        success: true, 
        message: 'Connected to ETD0180 calibration system',
        timestamp: new Date().toISOString()
    }));
}

function handleStartCapture(req, res) {
    const success = logProcessor.startCapture();
    res.writeHead(success ? 200 : 409, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ 
        success, 
        message: success ? 'Log capture started' : 'Capture already running',
        capturing: calibrationState.capturing
    }));
}

function handleStopCapture(req, res) {
    logProcessor.stopCapture();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ 
        success: true, 
        message: 'Log capture stopped',
        capturing: false
    }));
}

function handleLogs(req, res, query) {
    const limit = parseInt(query.limit) || 20;
    const recentLogs = calibrationState.logs.slice(-limit);
    
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        logs: recentLogs,
        total: calibrationState.logs.length,
        limit,
        capturing: calibrationState.capturing
    }));
}

function handleCalibration(req, res) {
    if (req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            ...calibrationState,
            timestamp: new Date().toISOString()
        }));
    } else if (req.method === 'POST') {
        let body = '';
        req.on('data', chunk => body += chunk);
        req.on('end', () => {
            try {
                const data = JSON.parse(body);
                // Store calibration data from HTML tool
                console.log('📥 Received calibration data from tool:', data);
                
                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ success: true, received: data }));
            } catch (error) {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: 'Invalid JSON' }));
            }
        });
    }
}

function handleSettings(req, res) {
    if (req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(calibrationState.settings));
    } else if (req.method === 'POST') {
        let body = '';
        req.on('data', chunk => body += chunk);
        req.on('end', () => {
            try {
                const newSettings = JSON.parse(body);
                Object.assign(calibrationState.settings, newSettings);
                
                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ 
                    success: true, 
                    settings: calibrationState.settings 
                }));
            } catch (error) {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: 'Invalid JSON' }));
            }
        });
    }
}

function handleMetrics(req, res) {
    const uptime = calibrationState.startTime ? 
        (Date.now() - new Date(calibrationState.startTime).getTime()) / 1000 : 0;
    
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        packets: calibrationState.packets,
        position: calibrationState.currentPosition,
        uptime,
        packetsPerSecond: uptime > 0 ? (calibrationState.packets.total / uptime).toFixed(2) : 0,
        movementRatio: calibrationState.packets.total > 0 ? 
            (calibrationState.packets.movement / calibrationState.packets.total * 100).toFixed(1) : 0
    }));
}

function handleRealtime(req, res) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
        ...calibrationState.realTimeData,
        currentPosition: calibrationState.currentPosition,
        timestamp: Date.now(),
        isLive: calibrationState.capturing
    }));
}

// Test results storage
let testResults = [];

function handleTestResult(req, res) {
    let body = '';
    
    req.on('data', chunk => {
        body += chunk.toString();
    });
    
    req.on('end', () => {
        try {
            const testResult = JSON.parse(body);
            
            // Add to test results storage
            testResults.push({
                ...testResult,
                receivedAt: new Date().toISOString()
            });
            
            // Keep only last 50 test results
            if (testResults.length > 50) {
                testResults = testResults.slice(-50);
            }
            
            console.log(`🧪 Test Result: Test ${testResult.testId} with ${testResult.dataPoints} data points`);
            
            // Detailed analysis for debugging
            if (testResult.dataPoints > 0) {
                console.log(`📊 Test ${testResult.testId} Analysis:`);
                console.log(`   Duration: ${testResult.duration}ms`);
                console.log(`   Data Points: ${testResult.dataPoints}`);
                
                const movements = testResult.data.filter(d => Math.abs(d.dx) > 0 || Math.abs(d.dy) > 0);
                const clicks = testResult.data.filter(d => d.leftBtn || d.rightBtn || d.middleBtn);
                
                console.log(`   Real Movements: ${movements.length}`);
                console.log(`   Button Clicks: ${clicks.length}`);
                
                if (movements.length > 0) {
                    const avgDx = movements.reduce((sum, d) => sum + Math.abs(d.dx), 0) / movements.length;
                    const avgDy = movements.reduce((sum, d) => sum + Math.abs(d.dy), 0) / movements.length;
                    console.log(`   Avg Movement: dx=${avgDx.toFixed(2)}, dy=${avgDy.toFixed(2)}`);
                }
            } else {
                console.log(`⚠️  Test ${testResult.testId}: NO REAL trackpad data captured!`);
            }
            
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ 
                success: true, 
                message: 'Test result received',
                testId: testResult.testId 
            }));
            
        } catch (error) {
            console.error('❌ Error processing test result:', error);
            res.writeHead(400, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ error: 'Invalid test result data' }));
        }
    });
}

function handle404(req, res) {
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ 
        error: 'Not found', 
        path: req.url,
        availableEndpoints: ['/api/status', '/api/logs', '/api/calibration', '/api/metrics', '/api/test-result']
    }));
}

// Start server
server.listen(PORT, HOST, () => {
    console.log('🚀 VoodooPS2 ETD0180 Claude API Server Started');
    console.log(`📡 Server running at http://${HOST}:${PORT}/`);
    console.log(`🔬 Calibration Tool: file:///Volumes/INTENSO/VoodooPS2Elan/calibration-tool.html`);
    console.log('');
    console.log('Claude can now access:');
    console.log('  • GET  /api/status     - Connection status');
    console.log('  • POST /api/start-capture - Start log monitoring');
    console.log('  • GET  /api/logs       - Recent ETD0180 logs');
    console.log('  • GET  /api/realtime   - Live trackpad data');
    console.log('  • GET  /api/metrics    - Statistics');
    console.log('  • POST /api/settings   - Update calibration');
    console.log('');
    console.log('🎯 Ready for ETD0180 calibration with live Claude monitoring!');
});

// Graceful shutdown
process.on('SIGINT', () => {
    console.log('\n⏹️  Shutting down API server...');
    logProcessor.stopCapture();
    server.close(() => {
        console.log('✅ API server stopped');
        process.exit(0);
    });
});

// Error handling
process.on('unhandledRejection', (reason, promise) => {
    console.error('Unhandled Rejection at:', promise, 'reason:', reason);
});

process.on('uncaughtException', (error) => {
    console.error('Uncaught Exception:', error);
    process.exit(1);
});