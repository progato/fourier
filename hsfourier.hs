import qualified Data.Vector.Unboxed as VU
import Data.Complex

type Signal = VU.Vector (Complex Double)

dot :: Complex Double -> Complex Double -> Complex Double
dot a b = a * conjugate b

dotS :: Signal -> Signal -> Complex Double
dotS a b = sum $ [(a VU.! i) `dot` (b VU.! i) | i <- [0..sz-1]] where
    sz = VU.length a

minus :: Signal -> Signal -> Signal
minus a b = VU.generate sz (\i -> a VU.! i - b VU.! i) where
    sz = VU.length a

-- | RMS of error signal.
error :: Signal -> Signal -> Double
error a b = sqrt (realPart (dotS e e)/fromIntegral sz) where
    e = a `minus` b
    sz = VU.length a

i = 0 :+ 1

dft :: Signal -> Signal
dft signal = VU.generate sz go where
    go :: Int -> Complex Double
    go k = sum $ map (f k) [0..sz-1]

    sz :: Int
    sz = VU.length signal

    f :: Int -> Int -> Complex Double
    f k n = (signal VU.! n) * exp (-i*2*pi*fromIntegral k*fromIntegral n/fromIntegral sz)

idft :: Signal -> Signal
idft spectrum = VU.generate sz go where
    go :: Int -> Complex Double
    go n = (sum $ map (f n) [0..sz-1])/fromIntegral sz

    sz :: Int
    sz = VU.length spectrum

    f :: Int -> Int -> Complex Double
    f n k = (spectrum VU.! k) * exp (i*2*pi*fromIntegral k*fromIntegral n/fromIntegral sz)

main = do
    let example = VU.replicate 1024 ((1 :+ 0) :: Complex Double)
    print $ dft example
