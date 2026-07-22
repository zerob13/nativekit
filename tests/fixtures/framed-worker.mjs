const encode = (value) => {
  const payload = Buffer.from(value)
  const header = Buffer.allocUnsafe(4)
  header.writeUInt32LE(payload.length)
  return Buffer.concat([header, payload])
}

if (process.argv.includes('--truncated')) {
  const header = Buffer.allocUnsafe(4)
  header.writeUInt32LE(8)
  process.stdout.write(
    Buffer.concat([header, Buffer.from('short')]),
    () => process.exit(0),
  )
} else if (process.argv.includes('--burst')) {
  process.stdout.write(
    Buffer.concat(
      Array.from({ length: 64 }, (_, index) => encode(`frame-${index}`)),
    ),
    () => process.exit(0),
  )
} else {
  const frame = encode('nativekit-worker')
  process.stdout.write(frame.subarray(0, 2))

  setTimeout(() => {
    process.stdout.write(frame.subarray(2))
    if (process.argv.includes('--wait')) {
      setInterval(() => {}, 1_000)
    } else {
      setTimeout(() => process.exit(0), 250)
    }
  }, 10)
}
