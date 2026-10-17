let epoch = 0
export const configReadEpoch = () => epoch
export const invalidateConfigReads = () => { epoch += 1 }
