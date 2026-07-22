let sequence = 0

function sendFrame() {
  const payload = Buffer.from(
    JSON.stringify({ sequence, message: 'nativekit worker frame' }),
  )
  sequence += 1
  const header = Buffer.allocUnsafe(4)
  header.writeUInt32LE(payload.length)
  process.stdout.write(Buffer.concat([header, payload]))
}

sendFrame()
setInterval(sendFrame, 750)
