//! Two-output covariance-domain optimal mixing, adapted from SAF cdf4sap
//! (Leo McCormack, ISC; see assets/NOTICE.txt). Only Q=identity and residual
//! synthesis, the OM configuration used by this project, are implemented.
use crate::{Complex32 as C, Error, Result};
use nalgebra::{Matrix2, Vector2};
pub type ComplexMatrix = Matrix2<C>;
pub type RealMatrix = Matrix2<f32>;
fn failed() -> Error {
    Error::RenderFailed("OM covariance decomposition failed")
}

pub fn complex_mix(
    cx: ComplexMatrix,
    cy: ComplexMatrix,
    regularization: f32,
) -> Result<(ComplexMatrix, RealMatrix)> {
    if !cx
        .iter()
        .chain(cy.iter())
        .all(|v| v.re.is_finite() && v.im.is_finite())
    {
        return Err(failed());
    }
    let sy = cy.try_svd(true, false, 1e-7, 64).ok_or_else(failed)?;
    let sx = cx.try_svd(true, false, 1e-7, 64).ok_or_else(failed)?;
    let uy = sy.u.ok_or_else(failed)?;
    let ux = sx.u.ok_or_else(failed)?;
    let sy = sy
        .singular_values
        .map(|v| C::new(v.max(2.23e-20).sqrt(), 0.));
    let sx = sx.singular_values.map(|v| v.max(2.23e-13).sqrt());
    let ky = uy * ComplexMatrix::from_diagonal(&sy);
    let kx = ux * ComplexMatrix::from_diagonal(&sx.map(|v| C::new(v, 0.)));
    let floor = sx.max() * regularization + 2.23e-13;
    let inverse =
        ComplexMatrix::from_diagonal(&sx.map(|v| C::new(1. / v.max(floor), 0.))) * ux.adjoint();
    let limit = cx[(0, 0)].norm().max(cx[(1, 1)].norm()) * 0.001 + 2.23e-13;
    let gain = ComplexMatrix::from_diagonal(&Vector2::new(
        C::new((cy[(0, 0)] / cx[(0, 0)].norm().max(limit)).sqrt().re, 0.),
        C::new((cy[(1, 1)] / cx[(1, 1)].norm().max(limit)).sqrt().re, 0.),
    ));
    let polar = (kx.adjoint() * gain.adjoint() * ky)
        .try_svd(true, true, 1e-7, 64)
        .ok_or_else(failed)?;
    let p = polar.v_t.ok_or_else(failed)?.adjoint() * polar.u.ok_or_else(failed)?.adjoint();
    let mixing = ky * p * inverse;
    let residual = (cy - mixing * cx * mixing.adjoint()).map(|v| v.re);
    if !mixing.iter().all(|v| v.re.is_finite() && v.im.is_finite())
        || !residual.iter().all(|v| v.is_finite())
    {
        return Err(failed());
    }
    Ok((mixing, residual))
}

pub fn real_mix(cx: RealMatrix, cy: RealMatrix, regularization: f32) -> Result<RealMatrix> {
    if !cx.iter().chain(cy.iter()).all(|v| v.is_finite()) {
        return Err(failed());
    }
    let sy = cy.try_svd(true, false, 1e-7, 64).ok_or_else(failed)?;
    let sx = cx.try_svd(true, false, 1e-7, 64).ok_or_else(failed)?;
    let uy = sy.u.ok_or_else(failed)?;
    let ux = sx.u.ok_or_else(failed)?;
    let sy = sy.singular_values.map(|v| v.max(2.23e-20).sqrt());
    let sx = sx.singular_values.map(|v| v.max(2.23e-20).sqrt());
    let ky = uy * RealMatrix::from_diagonal(&sy);
    let kx = ux * RealMatrix::from_diagonal(&sx);
    let floor = sx.max() * regularization + 2.23e-13;
    let inverse = RealMatrix::from_diagonal(&sx.map(|v| 1. / v.max(floor))) * ux.transpose();
    let limit = cx[(0, 0)].max(cx[(1, 1)]) * 0.001 + 2.23e-13;
    let gain = RealMatrix::from_diagonal(&Vector2::new(
        (cy[(0, 0)].max(2.23e-13) / cx[(0, 0)].max(limit)).sqrt(),
        (cy[(1, 1)].max(2.23e-13) / cx[(1, 1)].max(limit)).sqrt(),
    ));
    let polar = (kx.transpose() * gain.transpose() * ky)
        .try_svd(true, true, 1e-7, 64)
        .ok_or_else(failed)?;
    let p = polar.v_t.ok_or_else(failed)?.transpose() * polar.u.ok_or_else(failed)?.transpose();
    let mixing = ky * p * inverse;
    if !mixing.iter().all(|v| v.is_finite()) {
        return Err(failed());
    }
    Ok(mixing)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn full_rank_covariance_reconstruction() {
        let cx = ComplexMatrix::new(
            C::new(0.8, 0.),
            C::new(0.1, 0.2),
            C::new(0.1, -0.2),
            C::new(0.6, 0.),
        );
        let cy = ComplexMatrix::new(
            C::new(1.2, 0.),
            C::new(0.3, -0.1),
            C::new(0.3, 0.1),
            C::new(0.9, 0.),
        );
        let (m, residual) = complex_mix(cx, cy, 0.2).unwrap();
        assert!((m * cx * m.adjoint() - cy).norm() / cy.norm() < 1e-5);
        assert!(residual.norm() < 1e-5);
        let x = RealMatrix::new(0.8, 0.1, 0.1, 0.6);
        let y = RealMatrix::new(1.2, 0.3, 0.3, 0.9);
        let m = real_mix(x, y, 0.2).unwrap();
        assert!((m * x * m.transpose() - y).norm() / y.norm() < 1e-5);
    }
    #[test]
    fn silent_rank_deficient_and_tiny_inputs_remain_finite() {
        for power in [0., 1e-20, 1e-6, 1.] {
            let x = ComplexMatrix::from_element(C::new(power, 0.));
            let y = ComplexMatrix::identity() * C::new(power, 0.);
            let (m, r) = complex_mix(x, y, 0.2).unwrap();
            assert!(m.iter().all(|v| v.re.is_finite() && v.im.is_finite()));
            assert!(real_mix(RealMatrix::identity() * (power + 1e-5), r, 0.2).is_ok());
        }
    }
}
